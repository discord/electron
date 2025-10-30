#include "base/allocator/dispatcher/subsystem.h"
#include "base/allocator/partition_allocator/src/partition_alloc/partition_alloc.h"
#include "base/allocator/partition_allocator/src/partition_alloc/partition_alloc_allocation_data.h"
#include "base/allocator/partition_allocator/src/partition_alloc/pointers/raw_ptr_exclusion.h"
#include "base/allocator/partition_allocator/src/partition_alloc/shim/allocator_shim_default_dispatch_to_partition_alloc.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/hash/hash.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/safety_checks.h"
#include "base/no_destructor.h"
#include "base/sampling_heap_profiler/poisson_allocation_sampler.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/platform_thread.h"
#include "gin/handle.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/node_includes.h"
#include "third_party/blink/renderer/platform/wtf/allocator/partitions.h"

#include <Windows.h>
#include <psapi.h>

#include <algorithm>
#include <format>
#include <mutex>
#include <optional>
#include <semaphore>

namespace {

// Define these as nothing, chromium doesn't seem to use the Clang thread safety
// annotations
#undef GUARDED_BY
#define GUARDED_BY(x)
#undef REQUIRES
#define REQUIRES(x)
#undef ACQUIRED_AFTER
#define ACQUIRED_AFTER(x)

// Maximum number of call stack frames to track, this is relatively larger
// since these call stacks are deeper.
constexpr uint32_t MAX_FRAMES = 64;

// Maximum length of a PA typename to track, these can be a bit lengthy so
// allow them to be larger to capture the interesting info
constexpr uint32_t MAX_TYPENAME = 256;

// Thread-safe multi-producer/single-consumer queue that doesn't spin-wait when
// looking for new elements to pop. This has a static capacity, if it becomes
// full new elements attempted to be added will fail. This doesn't allocate
// after being constructed, so we can safely call it from a hooked allocation
// function.
template <typename T, size_t Capacity>
  requires std::movable<T>
class MPSCQueue {
 private:
  mutable std::mutex mutex_;
  std::array<T, Capacity> buffer_ GUARDED_BY(mutex_);
  std::counting_semaphore<Capacity> semaphore_{0};
  size_t head_ GUARDED_BY(mutex_) = 0;  // read position
  size_t tail_ GUARDED_BY(mutex_) = 0;  // write position
  std::atomic<bool> closed_ = false;

 public:
  MPSCQueue() = default;

  MPSCQueue(const MPSCQueue&) = delete;
  MPSCQueue& operator=(const MPSCQueue&) = delete;
  MPSCQueue(MPSCQueue&&) = delete;
  MPSCQueue& operator=(MPSCQueue&&) = delete;

  [[nodiscard]] bool push(T item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || is_full_unsafe()) {
      return false;
    }

    buffer_[tail_] = std::move(item);
    tail_ = (tail_ + 1) % Capacity;
    semaphore_.release();
    return true;
  }

  [[nodiscard]] std::optional<T> try_pop() {
    semaphore_.acquire();

    std::lock_guard<std::mutex> lock(mutex_);
    if (is_empty_unsafe()) {
      // Release the semaphore as we might've just closed
      semaphore_.release();
      return std::nullopt;
    }

    T item = std::move(buffer_[head_]);
    head_ = (head_ + 1) % Capacity;
    return item;
  }

  void close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    semaphore_.release();
  }

  bool is_closed() const { return closed_.load(); }

  bool empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_empty_unsafe();
  }

  bool full() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_full_unsafe();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_unsafe();
  }

  constexpr size_t capacity() const { return Capacity; }

 private:
  bool is_empty_unsafe() const REQUIRES(mutex_) { return head_ == tail_; }

  bool is_full_unsafe() const REQUIRES(mutex_) {
    return (tail_ + 1) % Capacity == head_;
  }

  size_t size_unsafe() const REQUIRES(mutex_) {
    return (tail_ + Capacity - head_) % Capacity;
  }
};

#pragma pack(push, 1)
struct HeapEvent {
  enum class EventType : unsigned char {
    UNKNOWN,
    ALLOCATION,
    DEALLOCATION,
  };

  int64_t address{0};

  // Optionally present if type = ALLOCATION
  uint64_t sizeBytes{0};
  std::array<void*, MAX_FRAMES> frames;
  int frameCount{0};
  std::array<char, MAX_TYPENAME> attribTypeName{};

  EventType type{EventType::UNKNOWN};
};
#pragma pack(pop)
static_assert(std::is_trivially_copyable_v<HeapEvent> == true);

// Simple thread-safe MPSC queue so we can do the bigger work on a background
// thread.
// Dynamically allocate this when installing hooks, it can take up a sizeable
// amount of memory.
using mpsc_queue_t = MPSCQueue<HeapEvent, 24 * 1024>;
static_assert(sizeof(mpsc_queue_t) ==
              19'390'504);  // This exists to provide an explicit view of how
                            // much memory this will consume
base::NoDestructor<std::unique_ptr<mpsc_queue_t>> g_workerQueue;

#pragma pack(push, 1)
struct TrackedAlloc {
  int64_t address{0};
  uint64_t sizeBytes{0};
  bool isCurrentlyAllocated{false};
  std::array<char, MAX_TYPENAME> attribTypeName;
  int64_t callstackHash{0};
};
static_assert(std::is_trivially_copyable_v<TrackedAlloc> == true);

struct TrackedReAlloc {
  TrackedAlloc alloc;
  int64_t oldAddress{0};
};
static_assert(std::is_trivially_copyable_v<TrackedReAlloc> == true);
#pragma pack(pop)

// Track our allocations in a static sized array. Store a queue with all
// available indexes in this array for a quick lookup for the next free slot.
constexpr int MAX_TRACKED_ALLOCATIONS = 32 * 1024;
using tracked_allocations_t = std::array<TrackedAlloc, MAX_TRACKED_ALLOCATIONS>;
base::NoDestructor<std::queue<int>> g_unallocatedIndex;
std::mutex g_trackedAllocationsMutex;
base::NoDestructor<std::unique_ptr<tracked_allocations_t>> g_trackedAllocations
    GUARDED_BY(g_trackedAllocationsMutex);
static_assert(sizeof(tracked_allocations_t) ==
              9'207'808);  // This exists to provide an explicit view of how
                           // much memory this will consume

// Default threshold size, JS side can override this though
std::atomic<int64_t> g_allocationThresholdKB{256};
std::atomic<int64_t> g_v8AllocationThresholdKB{256};

// Wrapped in base::NoDestructor to satisfy -Wexit-time-destructors. NOTE: this
// means the std::jthread destructor's automatic request_stop()+join() is
// SKIPPED at process exit. If profiling is still active at exit, the worker
// thread will continue running until the OS tears down the process. Callers
// that need clean shutdown must invoke DisableProfiling explicitly.
base::NoDestructor<std::jthread> g_workerThread;
std::atomic<bool> g_partitionAllocatorHooked{false};
std::atomic<bool> g_v8ProfilerRunning{false};

// The PoissonAllocationSampler's sampling interval is process-wide and shared
// with other clients (e.g. DevTools' memory agent). Save it when we enable so
// DisableProfiling can restore it, rather than leaving our interval in place
// for the rest of the process.
std::atomic<size_t> g_savedSamplingInterval{0};

// Setting if we've enabled call stack tracking
std::atomic<bool> g_enableCallStackTracking{false};

// Associate each memory address to the index in g_trackedAllocations, we want
// to quickly look up addresses when deallocating
base::NoDestructor<std::unordered_map<int64_t, int>> g_addressToSlotIndex
    GUARDED_BY(g_trackedAllocationsMutex);

// Cache absolute callstack addresses with a hash lookup
std::mutex g_cacheHashedCallstacksMutex
    ACQUIRED_AFTER(g_trackedAllocationsMutex);
base::NoDestructor<std::unordered_map<int64_t, std::array<void*, MAX_FRAMES>>>
    g_cacheHashedCallstacks GUARDED_BY(g_cacheHashedCallstacksMutex);

// Cache the typeName -> CodeId lookups
base::NoDestructor<std::unordered_map<std::string, std::optional<std::string>>>
    g_cacheModuleCodeId;

// Track if our queue was ever full and we had to drop events, useful in
// determining confidence in our data
std::atomic<bool> g_workerQueueDroppedEvent{false};

void trackAllocation(const TrackedAlloc& trackedAllocation) {
  const std::lock_guard<std::mutex> lock(g_trackedAllocationsMutex);
  assert(*g_trackedAllocations);

  // Find the first available index
  if (g_unallocatedIndex->size() == 0) {
    // No available index, drop tracking the allocation
    return;
  }

  const uint32_t availableSlotIndex = g_unallocatedIndex->front();
  assert(availableSlotIndex < MAX_TRACKED_ALLOCATIONS);
  g_unallocatedIndex->pop();

  (**g_trackedAllocations)[availableSlotIndex] = trackedAllocation;

  (*g_addressToSlotIndex)[trackedAllocation.address] = availableSlotIndex;
}

// Note: This is likely to be more on the hot path than trackAllocation
// because we can't immediately filter out any free's since their sizes
// aren't provided.
void trackDeallocation(int64_t address) {
  // Search our structures for the address and (if found) mark them as dirty
  const std::lock_guard<std::mutex> lock(g_trackedAllocationsMutex);
  assert(*g_trackedAllocations);

  const auto itr = g_addressToSlotIndex->find(address);
  if (itr == g_addressToSlotIndex->end()) {
    // We weren't tracking this address, so just skip
    return;
  }

  (**g_trackedAllocations)[itr->second].isCurrentlyAllocated = false;
  g_unallocatedIndex->push(itr->second);
  g_addressToSlotIndex->erase(itr);
}

bool getModuleNameAndBaseAddressFromAddress(
    const void* address,
    std::array<wchar_t, MAX_PATH>& moduleName,
    int64_t& baseAddress) {
  HMODULE hModule;

  if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(address), &hModule)) {
    return false;
  }

  // The hModule is actually the baseAddress for the module (win32
  // implementation detail)
  baseAddress = reinterpret_cast<int64_t>(hModule);

  if (GetModuleBaseName(GetCurrentProcess(), hModule, moduleName.data(),
                        moduleName.size())) {
    const int len = wcslen(moduleName.data()) + 1;
    return (_wcslwr_s(moduleName.data(), len) == 0);
  }

  return false;
}

// Return the frameCount computed
int computeRelativeAddresses(
    const std::array<void*, MAX_FRAMES>& absoluteAddresses,
    std::array<uint32_t, MAX_FRAMES>& relativeAddresses,
    std::array<std::array<wchar_t, MAX_PATH>, MAX_FRAMES>& frameModuleNames) {
  std::array<int64_t, MAX_FRAMES> baseAddresses;

  // We rely on zeroed addresses to mark where the end of the addr stack is,
  // so compute the frame count here
  int frameCount = 0;
  for (uint32_t i = 0; i < MAX_FRAMES; i++) {
    if (absoluteAddresses[i] == nullptr) {
      break;
    }
    frameCount += 1;
  }

  // Gather the module names and base addresses
  for (int i = 0; i < frameCount; i++) {
    if (!getModuleNameAndBaseAddressFromAddress(
            absoluteAddresses[i], frameModuleNames[i], baseAddresses[i])) {
      constexpr wchar_t kUnknown[] = L"<unknown_0>";
      auto dest_span = base::span(frameModuleNames[i]);
      size_t copy_size = std::min(dest_span.size(), std::size(kUnknown));
      std::copy_n(kUnknown, copy_size, dest_span.begin());

      baseAddresses[i] = 0;
    }
  }

  // Compute the relative addresses
  for (int i = 0; i < frameCount; i++) {
    if (baseAddresses[i] == 0) {
      // Invalid, track the same for our relative
      relativeAddresses[i] = 0;
    } else {
      // Narrow this to u32, offsets should not exceed 4GB
      relativeAddresses[i] = static_cast<uint32_t>(
          reinterpret_cast<int64_t>(absoluteAddresses[i]) - baseAddresses[i]);
    }
  }

  return frameCount;
}

std::optional<std::wstring> getModuleFullPath(const std::string& baseName) {
  HMODULE hModule = GetModuleHandleA(baseName.c_str());
  if (!hModule) {
    return std::nullopt;
  }

  wchar_t modulePath[MAX_PATH];
  if (!GetModuleFileName(hModule, modulePath, MAX_PATH)) {
    return std::nullopt;
  }

  return std::wstring(modulePath);
}

struct CodeInfo {
  std::string codeId;     // Code ID (timestamp + image size)
  uint32_t timestamp{0};  // PE timestamp
  uint32_t imageSize{0};  // PE image size
};

// Helper that parses a PE file
std::optional<CodeInfo> extractCodeInfo(const std::string& filename) {
  base::FilePath path = base::FilePath(base::UTF8ToWide(filename));
  const auto& optFileData = base::ReadFileToBytes(path);
  if (!optFileData) {
    return std::nullopt;
  }
  base::span<const uint8_t> fileData = *optFileData;

  CodeInfo result;

  // Sanity check sizes and parse the basic DOS and NT headers
  if (fileData.size() < sizeof(IMAGE_DOS_HEADER)) {
    return std::nullopt;
  }

  auto dosHeaderBytes = fileData.first(sizeof(IMAGE_DOS_HEADER));
  const IMAGE_DOS_HEADER* dosHeader =
      reinterpret_cast<const IMAGE_DOS_HEADER*>(dosHeaderBytes.data());
  if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
    return std::nullopt;
  }

  size_t nt_offset = static_cast<size_t>(dosHeader->e_lfanew);
  if (nt_offset + sizeof(IMAGE_NT_HEADERS) > fileData.size()) {
    return std::nullopt;
  }

  auto ntHeaderBytes =
      fileData.subspan(nt_offset).first(sizeof(IMAGE_NT_HEADERS));
  const IMAGE_NT_HEADERS* ntHeaders =
      reinterpret_cast<const IMAGE_NT_HEADERS*>(ntHeaderBytes.data());

  if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
    return std::nullopt;
  }

  // Extract Code ID components from PE header
  result.timestamp = ntHeaders->FileHeader.TimeDateStamp;
  result.imageSize = ntHeaders->OptionalHeader.SizeOfImage;

  // Code ID format seems to roughly be: timestamp (32-bit hex) + image size
  // (32-bit hex):
  // 1. Leading zeros are trimmed (at least from the image size)
  // 2. Image size hex is lower case, while timestamp hex string is uppercase
  result.codeId = std::format("{:08X}{:x}", result.timestamp, result.imageSize);

  return result;
}

// Retrieves a hex-string of the "Code ID" for the binary image.
std::optional<std::string> getModuleCodeId(
    const char (&moduleName)[MAX_TYPENAME]) {
  // For the module version parse the CodeId from the PE file.

  const auto itr = g_cacheModuleCodeId->find(moduleName);
  if (itr != g_cacheModuleCodeId->end()) {
    return itr->second;
  }

  const auto optModuleFullPath = getModuleFullPath(moduleName);
  std::optional<std::string> retVal{std::nullopt};
  if (optModuleFullPath) {
    const auto& moduleFullPath = base::WideToUTF8(*optModuleFullPath);
    const auto optModuleCodeInfo = extractCodeInfo(moduleFullPath);
    if (optModuleCodeInfo) {
      retVal = (*optModuleCodeInfo).codeId;
    }
  }

  (*g_cacheModuleCodeId)[moduleName] = retVal;

  return retVal;
}

// Hash support for callstack frames
struct VectorHash {
  int64_t operator()(const std::array<void*, MAX_FRAMES>& vec,
                     int count) const {
    size_t seed = vec.size();
    for (int i = 0; i < count; i++) {
      seed = base::HashCombine(seed, vec[i]);
    }
    return static_cast<int64_t>(seed);
  }
};

// Computes hash of the callstack and caches from the callstack of absolute
// addresses
int64_t cacheAndRetrieveCallstackHash(
    const std::array<void*, MAX_FRAMES>& absoluteAddresses,
    int frameCount) {
  // Build a hash of the callstack to more efficiently store
  VectorHash hasher;
  const auto callstackHash = hasher(absoluteAddresses, frameCount);

  // Cache hash and real callstack
  const std::lock_guard<std::mutex> lock(g_cacheHashedCallstacksMutex);
  const auto itr = g_cacheHashedCallstacks->find(callstackHash);
  if (itr == g_cacheHashedCallstacks->end()) {
    (*g_cacheHashedCallstacks)[callstackHash] = absoluteAddresses;
  }

  return callstackHash;
}

// Handles all of the bigger work of looking up module names when tracking new
// allocations. Additionally, we are allowed to dynamically allocate in this
// thread context.
void workerThread(std::stop_token stopToken) {
  base::PlatformThread::SetName("PerfPAMemoryWorker");
  while (!stopToken.stop_requested()) {
    if (auto evtOpt = (*g_workerQueue)->try_pop()) {
      const auto& evt = *evtOpt;

      switch (evt.type) {
        case HeapEvent::EventType::ALLOCATION: {
          TrackedAlloc trackedAllocation{
              evt.address, evt.sizeBytes, true, {}, 0,
          };

          auto dest_span = base::span(trackedAllocation.attribTypeName);
          auto src_span = base::span(evt.attribTypeName).first(MAX_TYPENAME);
          dest_span.copy_from(
              src_span.first(std::min(dest_span.size(), src_span.size())));

          if (g_enableCallStackTracking) {
            // Calculate the hash from the absolute addresses in the callstack
            // first, this is faster to determine if we've discovered this
            // callstack before, without translating absolute -> relative
            // addresses.
            trackedAllocation.callstackHash =
                cacheAndRetrieveCallstackHash(evt.frames, evt.frameCount);
          }

          trackAllocation(trackedAllocation);

          break;
        }

        case HeapEvent::EventType::DEALLOCATION:
          trackDeallocation(evt.address);
          break;

        case HeapEvent::EventType::UNKNOWN:
          // Ignore
          break;
      }
    }
  }
}

// Retrieves the current callstack, returns the number of frames captured
int captureStack(std::array<void*, MAX_FRAMES>& stackFrames) {
#if PA_BUILDFLAG(PA_ARCH_CPU_X86_64)
  CONTEXT context;
  RtlCaptureContext(&context);

  int frameCount = 0;
  while (context.Rip != 0 && frameCount < static_cast<int>(MAX_FRAMES)) {
    stackFrames[frameCount] = reinterpret_cast<void*>(context.Rip);
    frameCount += 1;

    PVOID imageBase;
    const PRUNTIME_FUNCTION runtimeFunction = RtlLookupFunctionEntry(
        context.Rip, reinterpret_cast<PDWORD64>(&imageBase), nullptr);

    if (!runtimeFunction) {
      // If we can't find the unwind info just bail out
      break;
    }

    PVOID handlerData;
    ULONG64 establisherFrame;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, reinterpret_cast<ULONG64>(imageBase),
                     context.Rip, runtimeFunction, &context, &handlerData,
                     &establisherFrame, nullptr);
  }

  // Set the remaining frames to null to mark them cleared
  for (int i = frameCount; i < static_cast<int>(MAX_FRAMES); i++) {
    stackFrames[i] = nullptr;
  }

  return frameCount;
#else
  // Ignore for non-x86/x64
  for (int i = 0; i < static_cast<int>(MAX_FRAMES); i++) {
    stackFrames[i] = nullptr;
  }
  return 0;
#endif
}

// Observes the PoissonAllocationSampler that Electron's memory_system installs
// on the PartitionAlloc hook, rather than owning the hook directly (which would
// collide with the sampler and trip PA_CHECK in SetObserverHooks). Registered
// while profiling is active via AddSamplesObserver.
class DiscordMemorySampler
    : public base::PoissonAllocationSampler::SamplesObserver {
 public:
  void SampleAdded(void* address,
                   size_t size,
                   size_t /*total*/,
                   base::allocator::dispatcher::AllocationSubsystem type,
                   const char* context) override {
    // The sampler also reports allocator-shim (malloc) samples; keep the
    // profiler's original PartitionAlloc-only behavior.
    if (type !=
        base::allocator::dispatcher::AllocationSubsystem::kPartitionAllocator) {
      return;
    }

    if (!*g_workerQueue || (*g_workerQueue)->is_closed()) {
      return;
    }

    // Simple filter to exclude smaller allocation sizes.
    if (static_cast<int64_t>(size) < (g_allocationThresholdKB * 1024)) {
      return;
    }

    HeapEvent heapEvent;
    heapEvent.type = HeapEvent::EventType::ALLOCATION;
    heapEvent.frameCount = captureStack(heapEvent.frames);
    heapEvent.address = reinterpret_cast<int64_t>(address);
    heapEvent.sizeBytes = size;

    // For PartitionAlloc samples |context| is the allocation's type name.
    if (context) {
      auto dest_span = base::span(heapEvent.attribTypeName);
      UNSAFE_BUFFERS(auto src_span = base::span(context, strlen(context));)
      size_t copy_size = std::min(dest_span.size() - 1, src_span.size());
      dest_span.first(copy_size).copy_from(src_span.first(copy_size));
      heapEvent.attribTypeName[copy_size] = '\0';
    }

    // Note: Since we offload the work of looking up module names to a thread
    //       it's possible (in rarer cases) that the culprit DLL may be unloaded
    //       by then.
    if (!(*g_workerQueue)->push(heapEvent)) {
      // If we fail to push into the queue (full) just drop tracking it.
      // We might be able to do some more in the future here, but it gets more
      // tricky to handle. Generally this occurs if the worker thread can't
      // process many (de)allocations fast enough.
      g_workerQueueDroppedEvent = true;
    }
  }

  void SampleRemoved(void* address) override {
    if (!*g_workerQueue || (*g_workerQueue)->is_closed()) {
      return;
    }

    HeapEvent heapEvent;
    heapEvent.type = HeapEvent::EventType::DEALLOCATION;
    heapEvent.address = reinterpret_cast<int64_t>(address);

    if (!(*g_workerQueue)->push(heapEvent)) {
      // If we fail to push into the queue (full) just drop tracking it
      g_workerQueueDroppedEvent = true;
    }
  }
};

base::NoDestructor<DiscordMemorySampler> g_sampler;

}  // namespace

namespace {

void EnableProfiling(gin::Arguments* args) {
  if (g_partitionAllocatorHooked) {
    // Partition allocator already hooked, nothing to do
    return;
  }

  gin_helper::Dictionary options;
  if (args->GetNext(&options)) {
    int64_t allocationThresholdKB;
    if (options.Get("allocationThresholdKB", &allocationThresholdKB)) {
      if (allocationThresholdKB >= 0) {
        g_allocationThresholdKB = allocationThresholdKB;
      }
    }

    bool enableCallStackTracking;
    if (options.Get("enableCallStackTracking", &enableCallStackTracking)) {
      g_enableCallStackTracking = enableCallStackTracking;
    }
  }

  // Reset unallocated
  {
    const std::lock_guard<std::mutex> lock(g_trackedAllocationsMutex);

    *g_trackedAllocations = std::make_unique<tracked_allocations_t>();

    g_addressToSlotIndex->clear();

    std::queue<int> emptyQueue;
    std::swap(*g_unallocatedIndex, emptyQueue);
    for (int i = 0; i < MAX_TRACKED_ALLOCATIONS; i++) {
      g_unallocatedIndex->push(i);
    }
  }

  g_workerQueueDroppedEvent = false;
  {
    const std::lock_guard<std::mutex> lockB(g_cacheHashedCallstacksMutex);
    g_cacheHashedCallstacks->clear();
  }

  // Open the queue, rebuild it
  *g_workerQueue = std::make_unique<mpsc_queue_t>();

  // Start the thread so it can immediately start working on the queue.
  *g_workerThread = std::jthread(workerThread);

  // Plug into the PoissonAllocationSampler that Electron's memory_system already
  // installed on the PartitionAlloc hook (kEnforce). Registering the observer is
  // what activates sampling. Sample at roughly the size we care about so large
  // allocations are captured with high probability; SampleAdded still applies
  // the exact threshold.
  auto* sampler = base::PoissonAllocationSampler::Get();

  // Sampling interval in bytes that are allocated before taking a sample
  const size_t samplingInterval =
      g_allocationThresholdKB > 0
          ? static_cast<size_t>(g_allocationThresholdKB) * 1024
          : 128 * 1024;
  // Save the existing interval so DisableProfiling can restore it; the sampler
  // is process-wide and shared with other clients (e.g. DevTools).
  g_savedSamplingInterval = sampler->SamplingInterval();
  sampler->SetSamplingInterval(samplingInterval);
  sampler->AddSamplesObserver(g_sampler.get());
  g_partitionAllocatorHooked = true;
}

void DisableProfiling(gin::Arguments* args) {
  if (!g_partitionAllocatorHooked) {
    // Partition allocator wasn't hooked, nothing to do
    return;
  }

  // Stop receiving samples before tearing down the queue. RemoveSamplesObserver
  // stops the sampler once its last observer is removed.
  auto* sampler = base::PoissonAllocationSampler::Get();
  sampler->RemoveSamplesObserver(g_sampler.get());

  // Restore the sampling interval we overwrote in EnableProfiling so other
  // clients of this process-wide sampler (e.g. DevTools) aren't left with ours.
  sampler->SetSamplingInterval(g_savedSamplingInterval);

  // Note: Some important cleanup ordering below to avoid deadlocks and crashes
  {
    // Request the thread to stop first as we want the thread loop to check this
    // after we signal the queue is closing
    g_workerThread->request_stop();

    // Close our queue first, otherwise the thread can get stuck waiting on
    // try_pop
    if (*g_workerQueue) {
      (*g_workerQueue)->close();
      // Don't destroy the queue, the thread below is still using it
    }

    if (g_workerThread->joinable()) {
      g_workerThread->join();
    }

    // Destroy this after the thread is joined, as it uses this queue
    g_workerQueue->reset();
  }

  g_trackedAllocations->reset();
  g_workerQueueDroppedEvent = false;

  const std::lock_guard<std::mutex> lockB(g_cacheHashedCallstacksMutex);
  g_cacheHashedCallstacks->clear();

  g_partitionAllocatorHooked = false;
}

struct TypeNameAllocation {
  uint32_t totalAllocKb{0};
  uint32_t allocCount{0};
};

v8::Local<v8::Value> GetTypeNameMemoryAllocated(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  gin::Dictionary result = gin::Dictionary::CreateEmpty(isolate);

  if (!g_partitionAllocatorHooked) {
    // Partition allocator wasn't hooked, nothing to do
    return gin::ConvertToV8(isolate, result);
  }

  const std::lock_guard<std::mutex> lock(g_trackedAllocationsMutex);
  assert(*g_trackedAllocations);

  std::unordered_map<std::string, TypeNameAllocation> memoryAllocated;

  for (const auto& kv : *g_addressToSlotIndex) {
    const auto& alloc = (**g_trackedAllocations)[kv.second];
    assert(alloc.isCurrentlyAllocated);

    const auto typeNameLen = std::min(static_cast<size_t>(MAX_TYPENAME),
                                      strlen(alloc.attribTypeName.data()));
    const auto& typeNameStr =
        std::string(alloc.attribTypeName.data(), typeNameLen);

    memoryAllocated[typeNameStr].totalAllocKb += alloc.sizeBytes / 1024;
    memoryAllocated[typeNameStr].allocCount += 1;
  }

  for (const auto& [typeName, allocation] : memoryAllocated) {
    gin::Dictionary entry = gin::Dictionary::CreateEmpty(isolate);
    entry.Set("total_allocation_kb", allocation.totalAllocKb);
    entry.Set("allocation_count", allocation.allocCount);
    result.Set(typeName, entry);
  }

  return gin::ConvertToV8(isolate, result);
}

struct CallstackAttributedAlloc {
  std::string typeName;
  std::vector<std::string> moduleNames;
  std::vector<std::string> moduleCodeIds;
  std::vector<uint32_t> relativeOffsets;
  uint32_t totalAllocKB;
};

template <size_t N>
void convertStringToUTF8(const wchar_t* s, char (&outString)[N]) {
  const auto& moduleNameUtf8 = base::WideToUTF8(s);
  auto dest_span = base::span(outString);

  size_t copy_size = std::min(moduleNameUtf8.size(), dest_span.size() - 1);
  std::copy_n(moduleNameUtf8.begin(), copy_size, dest_span.begin());
  std::fill(dest_span.begin() + copy_size, dest_span.end(), '\0');
}

v8::Local<v8::Value> GetTypeNameMemoryCallstacks(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  std::vector<gin::Dictionary> results;

  if (!g_partitionAllocatorHooked) {
    // Partition allocator wasn't hooked, nothing to do
    return gin::ConvertToV8(isolate, results);
  }

  // Extract the arguments
  gin::Dictionary options(isolate);
  if (!args->GetNext(&options)) {
    return gin::ConvertToV8(isolate, results);
  }

  std::string typeName;
  if (!options.Get("typeName", &typeName)) {
    return gin::ConvertToV8(isolate, results);
  }

  if (!g_enableCallStackTracking) {
    return gin::ConvertToV8(isolate, results);
  }

  std::vector<std::string> moduleNames;
  std::vector<std::string> moduleCodeIds;
  std::vector<uint32_t> relOffsets;
  moduleNames.reserve(MAX_FRAMES);
  moduleCodeIds.reserve(MAX_FRAMES);
  relOffsets.reserve(MAX_FRAMES);

  const std::lock_guard<std::mutex> lockA(g_trackedAllocationsMutex);
  const std::lock_guard<std::mutex> lockB(g_cacheHashedCallstacksMutex);

  // Find all the (callstack hash -> total allocation size kB) for this typename
  std::unordered_map<int64_t, uint32_t> memoryAllocatedPerCallstackHash;
  for (const auto& kv : *g_addressToSlotIndex) {
    const auto& alloc = (**g_trackedAllocations)[kv.second];
    assert(alloc.isCurrentlyAllocated);

    const auto typeNameLen = std::min(static_cast<size_t>(MAX_TYPENAME),
                                      strlen(alloc.attribTypeName.data()));
    const auto& typeNameStr =
        std::string(alloc.attribTypeName.data(), typeNameLen);
    if (typeNameStr == typeName) {
      memoryAllocatedPerCallstackHash[alloc.callstackHash] +=
          alloc.sizeBytes / 1024;
    }
  }

  results.reserve(memoryAllocatedPerCallstackHash.size());

  // For each hash compute the relative addresses and lookup the module names
  // for each frame
  for (const auto& [hash, totalAllocForCallstackKB] :
       memoryAllocatedPerCallstackHash) {
    moduleNames.clear();
    moduleCodeIds.clear();
    relOffsets.clear();

    gin::Dictionary entry = gin::Dictionary::CreateEmpty(isolate);
    entry.Set("type_name", typeName);
    entry.Set("total_alloc_kb", totalAllocForCallstackKB);

    if (hash == 0) {
      // Hash wasn't computed for this allocation, use empty arrays
      entry.Set("frame_module_names", moduleNames);
      entry.Set("frame_module_codeids", moduleCodeIds);
      entry.Set("frame_rel_offsets", relOffsets);
      results.push_back(entry);
      continue;
    }

    const auto itr = g_cacheHashedCallstacks->find(hash);
    if (itr == g_cacheHashedCallstacks->end()) {
      // Unexpected - skip this entry
      continue;
    }

    const std::array<void*, MAX_FRAMES>& frames = itr->second;

    // Gather the module names and relative addresses for each frame
    std::array<std::array<wchar_t, MAX_PATH>, MAX_FRAMES> frameModuleNames;
    for (auto& frame_name : frameModuleNames) {
      std::fill(frame_name.begin(), frame_name.end(), L'\0');
    }
    std::array<uint32_t, MAX_FRAMES> relativeAddresses;
    const int frameCount =
        computeRelativeAddresses(frames, relativeAddresses, frameModuleNames);

    // Build out our vector of module names, versions and relative offsets
    for (int i = 0; i < frameCount; i++) {
      char frameModuleNameUtf8[MAX_TYPENAME];
      convertStringToUTF8(frameModuleNames[i].data(), frameModuleNameUtf8);
      moduleNames.push_back(frameModuleNameUtf8);

      const auto& optModuleVersion = getModuleCodeId(frameModuleNameUtf8);
      if (optModuleVersion) {
        moduleCodeIds.push_back(*optModuleVersion);
      } else {
        moduleCodeIds.emplace_back("");
      }

      relOffsets.push_back(relativeAddresses[i]);
    }

    entry.Set("frame_module_names", moduleNames);
    entry.Set("frame_module_codeids", moduleCodeIds);
    entry.Set("frame_rel_offsets", relOffsets);

    results.push_back(entry);
  }

  return gin::ConvertToV8(isolate, results);
}

class TotalSizeCollector : public partition_alloc::PartitionStatsDumper {
 public:
  TotalSizeCollector() : total_size_(0) {}

  // Override the virtual methods
  void PartitionDumpTotals(
      const char* partition_name,
      const partition_alloc::PartitionMemoryStats* stats) override {
    total_size_ += stats->total_active_bytes;
  }

  void PartitionsDumpBucketStats(
      const char* partition_name,
      const partition_alloc::PartitionBucketMemoryStats* stats) override {
    // Optional: collect per-bucket stats if needed
  }

  size_t GetTotalSizeKB() const { return total_size_ / 1024; }

 private:
  size_t total_size_;
};

v8::Local<v8::Value> GetPartitionAllocatorStats(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  gin::Dictionary retVal = gin::Dictionary::CreateEmpty(isolate);

  size_t totalAllocatedKB = 0;

  // Dump malloc partition
  auto* malloc_root =
      allocator_shim::internal::PartitionAllocMalloc::Allocator();
  if (malloc_root) {
    TotalSizeCollector collector;
    malloc_root->DumpStats("malloc", true, false, &collector);
    retVal.Set("pa_malloc_kb",
               static_cast<int32_t>(collector.GetTotalSizeKB()));
    totalAllocatedKB += collector.GetTotalSizeKB();
  }

  // Original/aligned partition (used for aligned allocations)
  auto* original_root =
      allocator_shim::internal::PartitionAllocMalloc::OriginalAllocator();
  if (original_root) {
    TotalSizeCollector collector;
    original_root->DumpStats("original", true, false, &collector);
    retVal.Set("pa_malloc_original_kb",
               static_cast<int32_t>(collector.GetTotalSizeKB()));
    totalAllocatedKB += collector.GetTotalSizeKB();
  }

  auto* array_buffer_root = blink::Partitions::ArrayBufferPartition();
  if (array_buffer_root) {
    TotalSizeCollector collector;
    array_buffer_root->DumpStats("array_buffer", true, false, &collector);
    retVal.Set("pa_blink_array_buffer_kb",
               static_cast<int32_t>(collector.GetTotalSizeKB()));
    totalAllocatedKB += collector.GetTotalSizeKB();
  }

  auto* buffer_root = blink::Partitions::BufferPartition();
  if (buffer_root) {
    TotalSizeCollector collector;
    buffer_root->DumpStats("buffer", true, false, &collector);
    retVal.Set("pa_blink_buffer_kb",
               static_cast<int32_t>(collector.GetTotalSizeKB()));
    totalAllocatedKB += collector.GetTotalSizeKB();
  }

  retVal.Set("total_alloc_kb", static_cast<int32_t>(totalAllocatedKB));

  return gin::ConvertToV8(isolate, retVal);
}

namespace {

struct V8AllocFrame {
  std::string name;
  std::string scriptName;
  int lineNum;
  int colNum;

  // Size of the allocations for just this frame, excluding child allocations
  int64_t totalFrameAllocationBytes{0};
};

using Callstack = std::vector<V8AllocFrame>;

void gatherProfileCallstack(v8::Isolate* isolate,
                            const v8::AllocationProfile::Node* node,
                            Callstack currentCallstack,
                            std::vector<Callstack>& outHighMemCallstacks,
                            int64_t thresholdMinBytes) {
  int64_t totalAllocationBytes = 0;
  for (const auto& allocation : node->allocations) {
    totalAllocationBytes += allocation.size * allocation.count;
  }

  const auto scriptNameStr = v8::String::Utf8Value(isolate, node->script_name);
  const auto nameStr = v8::String::Utf8Value(isolate, node->name);

  // Scripts with kNoScriptId seem to be built-in V8 functions, mark it as such
  std::string scriptName =
      node->script_id == v8::UnboundScript::kNoScriptId
          ? "<builtin>"
          : (*scriptNameStr
                 ? std::string(*scriptNameStr, scriptNameStr.length())
                 : "<invalid_0>");

  V8AllocFrame frameInfo;
  if (*nameStr) {
    frameInfo.name = std::string(*nameStr, nameStr.length());
  }
  frameInfo.scriptName = std::move(scriptName);
  frameInfo.lineNum = node->line_number;
  frameInfo.colNum = node->column_number;
  frameInfo.totalFrameAllocationBytes = totalAllocationBytes;

  // Add our current frame the working callstack
  currentCallstack.push_back(frameInfo);

  // If our current frame's allocation bytes exceeds the threshold then add it
  if (frameInfo.totalFrameAllocationBytes >= thresholdMinBytes) {
    outHighMemCallstacks.push_back(currentCallstack);
  }

  // Work through our children too and have them populate the
  // outHighMemCallstacks if their frames (or descendent frames) exceed the min
  // threshold too
  for (const v8::AllocationProfile::Node* child : node->children) {
    gatherProfileCallstack(isolate, child, currentCallstack,
                           outHighMemCallstacks, thresholdMinBytes);
  }
}

std::vector<Callstack> gatherV8ProfileHighMem(
    v8::Isolate* isolate,
    const v8::AllocationProfile::Node* node,
    int64_t thresholdMinBytes) {
  Callstack currentCallstack;
  std::vector<Callstack> outHighMemCallstacks;
  outHighMemCallstacks.reserve(32);
  gatherProfileCallstack(isolate, node, currentCallstack, outHighMemCallstacks,
                         thresholdMinBytes);

  // Each callstack needs to be flipped
  for (auto& cs : outHighMemCallstacks) {
    std::reverse(cs.begin(), cs.end());
  }

  // Cleanup the name and script if empty
  for (auto& cs : outHighMemCallstacks) {
    for (auto& frame : cs) {
      if (frame.name.empty()) {
        frame.name = "<anonymous>";
      }

      if (frame.scriptName.empty()) {
        frame.scriptName = "<unknown_script>";
      }
    }
  }

  return outHighMemCallstacks;
}

}  // namespace

void EnableProfilingV8Heap(gin::Arguments* args) {
  if (g_v8ProfilerRunning) {
    return;
  }

  v8::Isolate* isolate = args->isolate();
  v8::HeapProfiler* profiler = isolate->GetHeapProfiler();

  // Extract the arguments
  int32_t sampleIntervalBytes{64 * 1024};
  int32_t stackDepth{64};

  gin_helper::Dictionary options;
  if (args->GetNext(&options)) {
    int64_t allocationThresholdKB;
    if (options.Get("allocationThresholdKB", &allocationThresholdKB)) {
      if (allocationThresholdKB >= 0) {
        g_v8AllocationThresholdKB = allocationThresholdKB;
      }
    }

    int32_t argSampleIntervalBytes;
    if (options.Get("sampleIntervalBytes", &argSampleIntervalBytes)) {
      if (argSampleIntervalBytes >= 0) {
        sampleIntervalBytes = argSampleIntervalBytes;
      }
    }

    int32_t argStackDepth;
    if (options.Get("stackDepth", &argStackDepth)) {
      if (argStackDepth >= 0) {
        stackDepth = argStackDepth;
      }
    }
  }

  profiler->StartSamplingHeapProfiler(sampleIntervalBytes, stackDepth);

  g_v8ProfilerRunning = true;
}

void DisableProfilingV8Heap(gin::Arguments* args) {
  if (!g_v8ProfilerRunning) {
    return;
  }

  v8::Isolate* isolate = args->isolate();
  v8::HeapProfiler* profiler = isolate->GetHeapProfiler();

  profiler->StopSamplingHeapProfiler();

  g_v8ProfilerRunning = false;
}

v8::Local<v8::Value> GetProfilerV8MemoryCallstacks(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  std::vector<gin::Dictionary> results;

  if (!g_v8ProfilerRunning) {
    // profiler isn't running, nothing to do
    return gin::ConvertToV8(isolate, results);
  }

  constexpr auto MAX_V8_FRAMES = 48;

  std::vector<std::string> frameName;
  std::vector<std::string> frameScriptName;
  std::vector<int64_t> frameAllocSize;
  std::vector<int32_t> frameLine;
  std::vector<int32_t> frameCol;
  frameName.reserve(MAX_V8_FRAMES);
  frameScriptName.reserve(MAX_V8_FRAMES);
  frameAllocSize.reserve(MAX_V8_FRAMES);
  frameLine.reserve(MAX_V8_FRAMES);
  frameCol.reserve(MAX_V8_FRAMES);

  results.reserve(32);

  // Best effort to force a GC, we want to clean up any non-live objects that
  // were profiled
  isolate->LowMemoryNotification();

  // Get a current profile, walk the allocation tree and transform it into
  // callstacks
  v8::HeapProfiler* profiler = isolate->GetHeapProfiler();
  v8::AllocationProfile* profile = profiler->GetAllocationProfile();
  if (!profile) {
    // profile isn't valid, nothing to do
    return gin::ConvertToV8(isolate, results);
  }

  const std::vector<Callstack> outHighMemCallstacks = gatherV8ProfileHighMem(
      isolate, profile->GetRootNode(), g_v8AllocationThresholdKB * 1024);
  delete profile;

  // Transform into a dictionary result
  for (const auto& cs : outHighMemCallstacks) {
    frameName.clear();
    frameScriptName.clear();
    frameAllocSize.clear();
    frameLine.clear();
    frameCol.clear();

    for (const auto& frame : cs) {
      frameName.push_back(frame.name);
      frameScriptName.push_back(frame.scriptName);
      frameAllocSize.push_back(frame.totalFrameAllocationBytes);
      frameLine.push_back(frame.lineNum);
      frameCol.push_back(frame.colNum);
    }

    gin::Dictionary entry = gin::Dictionary::CreateEmpty(isolate);
    entry.Set("frame_name", frameName);
    entry.Set("frame_script_name", frameScriptName);
    entry.Set("frame_alloc_size", frameAllocSize);
    entry.Set("frame_line", frameLine);
    entry.Set("frame_col", frameCol);

    results.push_back(entry);
  }

  return gin::ConvertToV8(isolate, results);
}

}  // namespace

namespace {

void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  gin_helper::Dictionary dict(isolate, exports);

  dict.SetMethod("enableProfiling", EnableProfiling);
  dict.SetMethod("disableProfiling", DisableProfiling);
  dict.SetMethod("getTypeNameMemoryAllocated", GetTypeNameMemoryAllocated);
  dict.SetMethod("getTypeNameMemoryCallstacks", GetTypeNameMemoryCallstacks);
  dict.SetMethod("getPartitionAllocatorStats", GetPartitionAllocatorStats);
  dict.SetMethod("enableProfilingV8Heap", EnableProfilingV8Heap);
  dict.SetMethod("disableProfilingV8Heap", DisableProfilingV8Heap);
  dict.SetMethod("getProfilerV8MemoryCallstacks",
                 GetProfilerV8MemoryCallstacks);
}

}  // namespace

NODE_LINKED_BINDING_CONTEXT_AWARE(electron_renderer_discord_memory_profiler,
                                  Initialize)
