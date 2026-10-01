#include "electron/discord/discord_frame_capture_manager.h"

#include <deque>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/sequence_checker.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_type.h"
#include "base/thread_annotations.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "electron/discord/discord_frame_capture_audio_ipc.h"
#include "media/audio/audio_input_device.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/base/audio_sample_types.h"
#include "media/base/video_frame.h"
#include "media/capture/mojom/video_capture_buffer.mojom.h"
#include "media/capture/mojom/video_capture_types.mojom.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/viz/privileged/mojom/compositing/frame_sink_video_capture.mojom.h"
#include "ui/gfx/gpu_memory_buffer_handle.h"

#if BUILDFLAG(IS_APPLE)
#include <IOSurface/IOSurfaceRef.h>
#endif

namespace discord {

namespace {

using media::electron::ElectronCapturedTexture;
using media::electron::ElectronObject;
using media::electron::ElectronPointer;
using media::electron::ElectronVideoPixelFormat;
using media::electron::ElectronVideoStatus;
using media::electron::IElectronCapturedFrame;
using media::electron::IElectronFrameCaptureSink;
using media::electron::RetainElectronVideoObject;

constexpr int kAudioSampleRate = 48000;
constexpr int kAudioFramesPerBuffer = kAudioSampleRate / 100;
constexpr base::TimeDelta kAudioRestartDelay = base::Seconds(1);

// Captures that stopped before a sink attached are remembered so a late sink
// still gets OnCaptureStopped. Bounded in case a sink never attaches.
constexpr size_t kMaxRememberedStops = 32;

// Frame delivery and all session and sink state live on this thread, away
// from the renderer main thread and its JS. It is never shut down.
scoped_refptr<base::SingleThreadTaskRunner> GetCaptureTaskRunner() {
  static base::NoDestructor<scoped_refptr<base::SingleThreadTaskRunner>>
      task_runner([] {
        static base::NoDestructor<base::Thread> thread("DiscordFrameCapture");
        base::Thread::Options options;
        options.thread_type = base::ThreadType::kPresentation;
        CHECK(thread->StartWithOptions(std::move(options)));
        return thread->task_runner();
      }());
  return *task_runner;
}

bool IsSupportedPixelFormat(::media::VideoPixelFormat format) {
  return format == ::media::PIXEL_FORMAT_ARGB ||
         format == ::media::PIXEL_FORMAT_ABGR ||
         format == ::media::PIXEL_FORMAT_RGBAF16 ||
         format == ::media::PIXEL_FORMAT_NV12;
}

class CapturedFrame : public ElectronObject<IElectronCapturedFrame> {
 public:
  CapturedFrame(
      ::media::mojom::VideoBufferHandlePtr data,
      const ::media::mojom::VideoFrameInfo& info,
      const electron::mojom::FrameCaptureParams& params,
      mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
          callbacks)
      : callbacks_(std::move(callbacks)) {
    FillInfo(info, params);
    if (data->is_gpu_memory_buffer_handle()) {
      handle_ = std::move(data->get_gpu_memory_buffer_handle());
      FillNativeHandle();
    }
#if BUILDFLAG(IS_LINUX)
    // Linux falls back to shared memory when the GPU cannot export native
    // pixmaps.
    else if (data->is_read_only_shmem_region()) {
      MapSharedMemory(data->get_read_only_shmem_region(), info);
    }
#endif
  }

  bool IsValid() const {
    return texture_.type != media::electron::kCapturedTextureNone;
  }

  const ElectronCapturedTexture* GetTexture() override { return &texture_; }

 private:
  // Dropping |callbacks_| returns the buffer to the capturer pool. That is
  // safe on any thread, so releasing a frame never waits on the capture
  // thread.
  ~CapturedFrame() override = default;

  void FillInfo(const ::media::mojom::VideoFrameInfo& info,
                const electron::mojom::FrameCaptureParams& params) {
    texture_.struct_size = sizeof(ElectronCapturedTexture);
    texture_.format = static_cast<ElectronVideoPixelFormat>(info.pixel_format);
    texture_.coded_width = info.coded_size.width();
    texture_.coded_height = info.coded_size.height();
    texture_.visible_x = info.visible_rect.x();
    texture_.visible_y = info.visible_rect.y();
    texture_.visible_width = info.visible_rect.width();
    texture_.visible_height = info.visible_rect.height();
    texture_.timestamp_us = info.metadata.reference_time
                                .value_or(base::TimeTicks::Now())
                                .since_origin()
                                .InMicroseconds();
    texture_.adapter_luid_low = params.adapter_luid_low;
    texture_.adapter_luid_high = params.adapter_luid_high;
  }

  void MapSharedMemory(const base::ReadOnlySharedMemoryRegion& region,
                       const ::media::mojom::VideoFrameInfo& info) {
    const auto format = info.pixel_format;
    const size_t planes = ::media::VideoFrame::NumPlanes(format);
    if (!planes || planes > std::size(texture_.planes)) {
      return;
    }
    mapping_ = region.Map();
    if (!mapping_.IsValid() ||
        mapping_.size() <
            ::media::VideoFrame::AllocationSize(format, info.coded_size)) {
      return;
    }

    base::span<media::electron::ElectronCapturedTexturePlane> texture_planes(
        texture_.planes);
    uint64_t offset = 0;
    for (size_t plane = 0; plane < planes; ++plane) {
      const size_t stride = ::media::VideoFrame::RowBytes(
          plane, format, info.coded_size.width());
      const size_t rows =
          ::media::VideoFrame::Rows(plane, format, info.coded_size.height());
      texture_planes[plane] = {-1, static_cast<uint32_t>(stride), offset,
                               static_cast<uint64_t>(stride * rows)};
      offset += stride * rows;
    }
    texture_.plane_count = planes;
    texture_.handle = const_cast<void*>(mapping_.memory());
    texture_.type = media::electron::kCapturedTextureSharedMemory;
  }

  // Also sets the stable per-pool-buffer identity consumers use to cache
  // imported resources.
  void FillNativeHandle() {
#if BUILDFLAG(IS_WIN)
    if (handle_.type == gfx::DXGI_SHARED_HANDLE) {
      texture_.type = media::electron::kCapturedTextureDxgiNtHandle;
      texture_.handle = handle_.dxgi_handle().buffer_handle();
      const auto& token = handle_.dxgi_handle().token();
      texture_.buffer_id =
          token->GetHighForSerialization() ^ token->GetLowForSerialization();
    }
#elif BUILDFLAG(IS_APPLE)
    if (handle_.type == gfx::IO_SURFACE_BUFFER && handle_.io_surface()) {
      texture_.type = media::electron::kCapturedTextureIOSurface;
      texture_.handle = handle_.io_surface().get();
      texture_.buffer_id = IOSurfaceGetID(handle_.io_surface().get());
    }
#elif BUILDFLAG(IS_LINUX)
    if (handle_.type == gfx::NATIVE_PIXMAP) {
      const auto& pixmap = handle_.native_pixmap_handle();
      texture_.type = media::electron::kCapturedTextureDmaBuf;
      texture_.modifier = pixmap.modifier;
      base::span<media::electron::ElectronCapturedTexturePlane> texture_planes(
          texture_.planes);
      for (const auto& plane : pixmap.planes) {
        if (texture_.plane_count == texture_planes.size()) {
          break;
        }
        texture_planes[texture_.plane_count++] = {
            plane.fd.get(), plane.stride, plane.offset, plane.size};
      }
    }
#endif
  }

  mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
      callbacks_;
  gfx::GpuMemoryBufferHandle handle_;
  base::ReadOnlySharedMemoryMapping mapping_;
  ElectronCapturedTexture texture_ = {};
};

class FrameCaptureSession;

// Only used on the capture thread, so it needs no locking and sink callbacks
// never run under a lock.
class SinkRegistry {
 public:
  SinkRegistry() { DETACH_FROM_SEQUENCE(sequence_checker_); }

  bool AddSession(const std::string& capture_id, FrameCaptureSession* session);
  void RemoveSession(const std::string& capture_id, const std::string& reason);
  void SetSink(const std::string& capture_id,
               ElectronPointer<IElectronFrameCaptureSink> sink);
  void Deliver(const std::string& capture_id,
               ElectronPointer<IElectronCapturedFrame> frame);

 private:
  struct Entry {
    raw_ptr<FrameCaptureSession> session = nullptr;
    ElectronPointer<IElectronFrameCaptureSink> sink;
    // The latest frame of a session that has no sink yet.
    ElectronPointer<IElectronCapturedFrame> held;
    // Set when the session stopped before a sink attached.
    std::optional<std::string> stop_reason;
  };

  void ForgetStop(const std::string& capture_id);
  void ForgetOldStops();

  SEQUENCE_CHECKER(sequence_checker_);

  std::map<std::string, Entry> entries_;
  // Holds an id exactly while its entry has a |stop_reason|, oldest first.
  std::deque<std::string> remembered_stops_;
};

SinkRegistry& GetSinkRegistry() {
  static base::NoDestructor<SinkRegistry> registry;
  return *registry;
}

// One FrameCapture session, living on the capture thread. Frames arrive
// straight from the GPU process. Audio is read from the audio service on a
// realtime thread owned by |audio_device_| and handed to the sink there.
class FrameCaptureSession : public electron::mojom::FrameCaptureSession,
                            public viz::mojom::FrameSinkVideoConsumer,
                            public ::media::AudioCapturerSource::CaptureCallback {
 public:
  static void Create(
      const std::string& capture_id,
      electron::mojom::FrameCaptureParamsPtr params,
      mojo::PendingReceiver<electron::mojom::FrameCaptureSession> receiver,
      mojo::PendingRemote<electron::mojom::FrameCaptureHost> host) {
    auto* session = new FrameCaptureSession(capture_id, std::move(params));
    if (!GetSinkRegistry().AddSession(capture_id, session)) {
      // Dropping the pipes makes the browser stop with "consumer-gone".
      LOG(ERROR) << "Frame capture " << capture_id << " is already bound";
      delete session;
      return;
    }
    session->Bind(std::move(receiver), std::move(host));
  }

  FrameCaptureSession(const FrameCaptureSession&) = delete;
  FrameCaptureSession& operator=(const FrameCaptureSession&) = delete;

  // Called by the registry whenever the attached sink changes.
  void SetAudioSink(IElectronFrameCaptureSink* sink) {
    ElectronPointer<IElectronFrameCaptureSink> old;
    {
      base::AutoLock auto_lock(audio_sink_lock_);
      old = std::move(audio_sink_);
      audio_sink_ = RetainElectronVideoObject(sink);
    }
  }

 private:
  FrameCaptureSession(const std::string& capture_id,
                      electron::mojom::FrameCaptureParamsPtr params)
      : capture_id_(capture_id), params_(std::move(params)) {
    weak_this_ = weak_factory_.GetWeakPtr();
  }

  ~FrameCaptureSession() override = default;

  void Bind(
      mojo::PendingReceiver<electron::mojom::FrameCaptureSession> receiver,
      mojo::PendingRemote<electron::mojom::FrameCaptureHost> host) {
    receiver_.Bind(std::move(receiver));
    receiver_.set_disconnect_handler(
        base::BindOnce(&FrameCaptureSession::Shutdown, base::Unretained(this),
                       std::string("stopped")));
    host_.Bind(std::move(host));
  }

  // electron::mojom::FrameCaptureSession
  void AttachVideoConsumer(
      mojo::PendingReceiver<viz::mojom::FrameSinkVideoConsumer> consumer)
      override {
    consumer_receiver_.reset();
    consumer_receiver_.Bind(std::move(consumer));
  }

  void StartAudio() override {
    audio_enabled_ = true;
    StartAudioDevice();
  }

  void StopAudio() override {
    audio_enabled_ = false;
    audio_restart_timer_.Stop();
    StopAudioDevice();
  }

  void Stop(const std::string& reason) override { Shutdown(reason); }

  // viz::mojom::FrameSinkVideoConsumer
  void OnFrameCaptured(
      ::media::mojom::VideoBufferHandlePtr data,
      ::media::mojom::VideoFrameInfoPtr info,
      const gfx::Rect& content_rect,
      mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
          callbacks) override {
    if (!IsSupportedPixelFormat(info->pixel_format)) {
      return;
    }
    auto* captured = new CapturedFrame(std::move(data), *info, *params_,
                                       std::move(callbacks));
    ElectronPointer<IElectronCapturedFrame> frame = captured;
    if (!captured->IsValid()) {
      return;
    }

    const gfx::Size size = info->visible_rect.size();
    if (size != last_frame_size_) {
      last_frame_size_ = size;
      host_->OnResized(size);
    }
    GetSinkRegistry().Deliver(capture_id_, std::move(frame));
  }

  void OnNewCaptureVersion(
      const ::media::CaptureVersion& capture_version) override {}
  void OnFrameWithEmptyRegionCapture() override {}
  void OnStopped() override {}
  void OnLog(const std::string& message) override {}

  // ::media::AudioCapturerSource::CaptureCallback. Capture() runs on the
  // realtime audio thread; OnCaptureError() may too.
  void Capture(const ::media::AudioBus* audio_source,
               base::TimeTicks audio_capture_time,
               const ::media::AudioGlitchInfo& glitch_info,
               double volume) override {
    ElectronPointer<IElectronFrameCaptureSink> sink;
    {
      base::AutoLock auto_lock(audio_sink_lock_);
      sink = RetainElectronVideoObject(
          static_cast<IElectronFrameCaptureSink*>(audio_sink_));
    }
    if (!sink) {
      return;
    }
    const int channels = audio_source->channels();
    const int frames = audio_source->frames();
    interleaved_.resize(static_cast<size_t>(channels) * frames);
    audio_source->ToInterleaved<::media::Float32SampleTypeTraits>(interleaved_);
    sink->OnCapturedAudio(
        interleaved_.data(), static_cast<uint32_t>(frames),
        static_cast<uint32_t>(channels), kAudioSampleRate,
        (audio_capture_time - base::TimeTicks()).InMicroseconds());
  }

  void OnCaptureError(::media::AudioCapturerSource::ErrorCode code,
                      const std::string& message) override {
    LOG(ERROR) << "Frame capture audio error: " << message;
    GetCaptureTaskRunner()->PostTask(
        FROM_HERE,
        base::BindOnce(&FrameCaptureSession::RestartAudio, weak_this_));
  }

  void OnCaptureMuted(bool is_muted) override {}

  void StartAudioDevice() {
    if (audio_device_ || !audio_enabled_ || stopped_) {
      return;
    }
    ::media::AudioParameters params(
        ::media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
        ::media::ChannelLayoutConfig::Stereo(), kAudioSampleRate,
        kAudioFramesPerBuffer);
    audio_device_ = base::MakeRefCounted<::media::AudioInputDevice>(
        std::make_unique<FrameCaptureAudioIPC>(host_.get()),
        ::media::AudioInputDevice::kLoopback,
        ::media::AudioInputDevice::DeadStreamDetection::kDisabled);
    audio_device_->Initialize(params, this);
    audio_device_->Start();
  }

  void StopAudioDevice() {
    if (!audio_device_) {
      return;
    }
    // Joins the audio thread, so no Capture() call can follow.
    audio_device_->Stop();
    audio_device_.reset();
  }

  // The stream dies with the audio service, or if the browser refused it
  // while a StopAudio() was in flight; retry while audio is still wanted.
  void RestartAudio() {
    StopAudioDevice();
    if (audio_enabled_ && !stopped_) {
      audio_restart_timer_.Start(FROM_HERE, kAudioRestartDelay, this,
                                 &FrameCaptureSession::StartAudioDevice);
    }
  }

  void Shutdown(const std::string& reason) {
    if (stopped_) {
      return;
    }
    stopped_ = true;

    // Frames still queued from the GPU process are dropped, which returns
    // their buffers, and the audio thread is joined, so nothing reaches the
    // sink after OnCaptureStopped.
    consumer_receiver_.reset();
    audio_restart_timer_.Stop();
    StopAudioDevice();
    SetAudioSink(nullptr);
    GetSinkRegistry().RemoveSession(capture_id_, reason);
    delete this;
  }

  const std::string capture_id_;
  const electron::mojom::FrameCaptureParamsPtr params_;

  mojo::Receiver<electron::mojom::FrameCaptureSession> receiver_{this};
  mojo::Remote<electron::mojom::FrameCaptureHost> host_;
  mojo::Receiver<viz::mojom::FrameSinkVideoConsumer> consumer_receiver_{this};
  gfx::Size last_frame_size_;
  bool audio_enabled_ = false;
  bool stopped_ = false;

  scoped_refptr<::media::AudioInputDevice> audio_device_;
  base::OneShotTimer audio_restart_timer_;
  // Only touched by the audio thread, which is joined before it is restarted.
  std::vector<float> interleaved_;

  base::Lock audio_sink_lock_;
  ElectronPointer<IElectronFrameCaptureSink> audio_sink_
      GUARDED_BY(audio_sink_lock_);

  // Created up front so the audio thread can post with it.
  base::WeakPtr<FrameCaptureSession> weak_this_;
  base::WeakPtrFactory<FrameCaptureSession> weak_factory_{this};
};

bool SinkRegistry::AddSession(const std::string& capture_id,
                              FrameCaptureSession* session) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Entry& entry = entries_[capture_id];
  if (entry.session) {
    return false;
  }
  entry.session = session;
  if (entry.stop_reason) {
    entry.stop_reason.reset();
    ForgetStop(capture_id);
  }
  if (entry.sink) {
    session->SetAudioSink(entry.sink);
  }
  return true;
}

void SinkRegistry::RemoveSession(const std::string& capture_id,
                                 const std::string& reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = entries_.find(capture_id);
  if (it == entries_.end()) {
    return;
  }
  Entry& entry = it->second;
  entry.session = nullptr;
  entry.held = nullptr;
  if (entry.sink) {
    ElectronPointer<IElectronFrameCaptureSink> sink = std::move(entry.sink);
    entries_.erase(it);
    sink->OnCaptureStopped(reason.c_str());
    return;
  }
  entry.stop_reason = reason;
  remembered_stops_.push_back(capture_id);
  ForgetOldStops();
}

void SinkRegistry::SetSink(const std::string& capture_id,
                           ElectronPointer<IElectronFrameCaptureSink> sink) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = entries_.find(capture_id);
  if (it == entries_.end()) {
    if (!sink) {
      return;
    }
    it = entries_.emplace(capture_id, Entry()).first;
  }
  Entry& entry = it->second;

  if (entry.stop_reason) {
    const std::string reason = std::move(*entry.stop_reason);
    entries_.erase(it);
    ForgetStop(capture_id);
    if (sink) {
      sink->OnCaptureStopped(reason.c_str());
    }
    return;
  }

  ElectronPointer<IElectronFrameCaptureSink> old = std::move(entry.sink);
  entry.sink = std::move(sink);
  IElectronFrameCaptureSink* current = entry.sink;
  ElectronPointer<IElectronCapturedFrame> held = std::move(entry.held);
  if (entry.session) {
    entry.session->SetAudioSink(current);
  } else if (!current) {
    entries_.erase(it);
  }

  // A page that isn't animating may never produce another frame, so the one
  // captured before the sink attached is what seeds it.
  if (current && held) {
    current->OnCapturedFrame(held);
  }
}

void SinkRegistry::Deliver(const std::string& capture_id,
                           ElectronPointer<IElectronCapturedFrame> frame) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = entries_.find(capture_id);
  if (it == entries_.end() || !it->second.session) {
    return;
  }
  Entry& entry = it->second;
  if (!entry.sink) {
    entry.held = std::move(frame);
    return;
  }
  entry.sink->OnCapturedFrame(frame);
}

void SinkRegistry::ForgetStop(const std::string& capture_id) {
  std::erase(remembered_stops_, capture_id);
}

void SinkRegistry::ForgetOldStops() {
  while (remembered_stops_.size() > kMaxRememberedStops) {
    auto it = entries_.find(remembered_stops_.front());
    remembered_stops_.pop_front();
    if (it != entries_.end() && it->second.stop_reason &&
        !it->second.session) {
      entries_.erase(it);
    }
  }
}

}  // namespace

DiscordFrameCaptureManager::DiscordFrameCaptureManager() = default;
DiscordFrameCaptureManager::~DiscordFrameCaptureManager() = default;

// static
void DiscordFrameCaptureManager::BindCapture(
    const std::string& capture_id,
    electron::mojom::FrameCaptureParamsPtr params,
    mojo::PendingReceiver<electron::mojom::FrameCaptureSession> session,
    mojo::PendingRemote<electron::mojom::FrameCaptureHost> host) {
  GetCaptureTaskRunner()->PostTask(
      FROM_HERE,
      base::BindOnce(&FrameCaptureSession::Create, capture_id,
                     std::move(params), std::move(session), std::move(host)));
}

ElectronVideoStatus DiscordFrameCaptureManager::SetSink(
    char const* captureId,
    IElectronFrameCaptureSink* sink) {
  if (!captureId || !*captureId) {
    return ElectronVideoStatus::Failure;
  }
  // Applied on the capture thread, so it never blocks the caller and is safe
  // to call from inside a sink callback.
  GetCaptureTaskRunner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](const std::string& capture_id,
             ElectronPointer<IElectronFrameCaptureSink> sink) {
            GetSinkRegistry().SetSink(capture_id, std::move(sink));
          },
          std::string(captureId), RetainElectronVideoObject(sink)));
  return ElectronVideoStatus::Success;
}

}  // namespace discord
