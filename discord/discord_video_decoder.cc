#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif

#include "electron/discord/discord_video_decoder.h"

#include "base/location.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread_restrictions.h"
#include "content/renderer/render_thread_impl.h"
#include "media/base/media_log.h"
#include "media/base/media_util.h"
#include "media/base/video_decoder_config.h"
#include "media/base/video_types.h"
#include "media/video/gpu_video_accelerator_factories.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cross_thread_task.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_copier_base.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/wtf/deque.h"

namespace discord {
namespace media {
namespace electron {

namespace {
using InitCB = CrossThreadOnceFunction<void(ElectronVideoStatus)>;

// Arbitrary, matches RTCVideoDecoderAdapter.
const gfx::Size kDefaultSize(640, 480);

// This is kind of an std::promise work-alike since chromium doesn't allow such
// things
template <typename T>
class WaitableSharedObject
    : public base::RefCountedThreadSafe<WaitableSharedObject<T>> {
 public:
  WaitableSharedObject<T>()
      : event_(base::WaitableEvent::ResetPolicy::MANUAL,
               base::WaitableEvent::InitialState::NOT_SIGNALED) {}

  void FinishWait(T value) {
    value_ = std::move(value);
    event_.Signal();
  }

  std::optional<T> TimedWait(base::TimeDelta delta) {
    if (!event_.TimedWait(delta)) {
      return std::nullopt;
    }
    return value_;
  }

  T Wait() {
    event_.Wait();
    return *value_;
  }

 private:
  friend class base::RefCountedThreadSafe<WaitableSharedObject<T>>;
  ~WaitableSharedObject<T>() = default;

  base::WaitableEvent event_;
  std::optional<T> value_;
};

void OnRequestOverlayInfo(bool decoder_requires_restart_for_overlay,
                          ::media::ProvideOverlayInfoCB overlay_info_cb) {
  // Android overlays are not supported.
  if (overlay_info_cb)
    std::move(overlay_info_cb).Run(::media::OverlayInfo());
}

struct GpuFactoriesAndTaskRunner {
  ::media::GpuVideoAcceleratorFactories* gpu_factories{nullptr};
  scoped_refptr<base::SequencedTaskRunner> media_task_runner{nullptr};
};

void GetGpuFactoriesOnRenderThread(
    CrossThreadOnceFunction<void(GpuFactoriesAndTaskRunner)> factoriesCb) {
  auto* gpu_factories = content::RenderThreadImpl::current()->GetGpuFactories();

  if (!gpu_factories) {
    std::move(factoriesCb).Run(GpuFactoriesAndTaskRunner{});
    return;
  }

  std::move(factoriesCb)
      .Run(GpuFactoriesAndTaskRunner{
          .gpu_factories = gpu_factories,
          .media_task_runner = gpu_factories->GetTaskRunner(),
      });
}
}  // namespace

class DiscordVideoDecoderMediaThread {
 public:
  DiscordVideoDecoderMediaThread(
      ::media::VideoDecoderConfig config_,
      ElectronVideoSink* video_sink,
      void* user_data,
      ::media::GpuVideoAcceleratorFactories* gpu_factories,
      scoped_refptr<base::SequencedTaskRunner> media_task_runner);

  DiscordVideoDecoderMediaThread(const DiscordVideoDecoderMediaThread&) =
      delete;
  DiscordVideoDecoderMediaThread& operator=(
      const DiscordVideoDecoderMediaThread&) = delete;

  ElectronVideoStatus Initialize();
  ElectronVideoStatus SubmitBuffer(IElectronBuffer* buffer,
                                   uint32_t timestamp,
                                   ElectronVideoDecodeErrorCB* errorCB,
                                   void* userData);
  void DeleteSoon();

 private:
  void InitializeOnMediaThread(InitCB init_cb);
  static void OnInitializeDone(base::OnceCallback<void(ElectronVideoStatus)> cb,
                               ::media::DecoderStatus status);
  void DecodeOnMediaThread();
  void OnDecodeDone(::media::DecoderStatus status);
  void OnOutput(scoped_refptr<::media::VideoFrame> frame);

  ::media::VideoDecoderConfig config_;
  DISCORD_RAW_PTR_EXCLUSION void* user_data_;
  DISCORD_RAW_PTR_EXCLUSION ::media::GpuVideoAcceleratorFactories*
      gpu_factories_{};
  std::unique_ptr<::media::MediaLog> media_log_;
  std::unique_ptr<::media::VideoDecoder> video_decoder_;
  scoped_refptr<base::SequencedTaskRunner> media_task_runner_;
  int32_t outstanding_decode_requests_{0};

  // shared state
  base::Lock lock_;
  std::optional<std::string> first_error_ GUARDED_BY(lock_){std::nullopt};
  WTF::Deque<scoped_refptr<::media::DecoderBuffer>> pending_buffers_
      GUARDED_BY(lock_);
  ElectronVideoSink* video_sink_ GUARDED_BY(lock_);
  base::WeakPtr<DiscordVideoDecoderMediaThread> weak_this_;
  base::WeakPtrFactory<DiscordVideoDecoderMediaThread> weak_this_factory_{this};
};

DiscordVideoDecoderMediaThread::DiscordVideoDecoderMediaThread(
    ::media::VideoDecoderConfig config,
    ElectronVideoSink* video_sink,
    void* user_data,
    ::media::GpuVideoAcceleratorFactories* gpu_factories,
    scoped_refptr<base::SequencedTaskRunner> media_task_runner)
    : config_(std::move(config)),
      user_data_(user_data),
      gpu_factories_(gpu_factories),
      media_task_runner_(std::move(media_task_runner)),
      video_sink_(video_sink) {
  weak_this_ = weak_this_factory_.GetWeakPtr();
}

void DiscordVideoDecoderMediaThread::DeleteSoon() {
  {
    base::AutoLock auto_lock(lock_);
    pending_buffers_.clear();
    video_sink_ = nullptr;
  }
  DCHECK(media_task_runner_ != nullptr);
  media_task_runner_->DeleteSoon(FROM_HERE, this);
}

ElectronVideoStatus DiscordVideoDecoderMediaThread::Initialize() {
  // This is gross, but the non "ForTesting" version of this requires an
  // explicit friend class declaration, and I don't want to modify base just to
  // add us to the whitelist. It might be worth just moving the blocking wait
  // out of this code and into the consumer in Discord.
  base::ScopedAllowBaseSyncPrimitivesForTesting allow_wait;
  auto waiter =
      base::MakeRefCounted<WaitableSharedObject<ElectronVideoStatus>>();  //
  InitCB init_cb = CrossThreadBindOnce(
      &WaitableSharedObject<ElectronVideoStatus>::FinishWait, waiter);

  if (blink::PostCrossThreadTask(
          *media_task_runner_.get(), FROM_HERE,
          CrossThreadBindOnce(
              &DiscordVideoDecoderMediaThread::InitializeOnMediaThread,
              CrossThreadUnretained(this), std::move(init_cb)))) {
    // TODO(crbug.com/1076817) Remove if a root cause is found.
    // TODO(eiz): ya, I dunno, I stole it from them.
    // TODO(aconverse): Five years laters, it's still there :(
    auto result = waiter->TimedWait(base::Seconds(10));
    return result.value_or(ElectronVideoStatus::TimedOut);
  }
  return ElectronVideoStatus::Failure;
}

void DiscordVideoDecoderMediaThread::InitializeOnMediaThread(InitCB init_cb) {
  DVLOG(3) << __func__;
  DCHECK(media_task_runner_->RunsTasksInCurrentSequence());

  if (media_log_ || video_decoder_) {
    blink::PostCrossThreadTask(
        *media_task_runner_.get(), FROM_HERE,
        CrossThreadBindOnce(std::move(init_cb),
                            ElectronVideoStatus::InvalidState));
    return;
  }

  media_log_ = std::make_unique<::media::NullMediaLog>();
  video_decoder_ = gpu_factories_->CreateVideoDecoder(
      media_log_.get(), WTF::BindRepeating(&OnRequestOverlayInfo));

  if (!video_decoder_) {
    blink::PostCrossThreadTask(
        *media_task_runner_.get(), FROM_HERE,
        CrossThreadBindOnce(std::move(init_cb), ElectronVideoStatus::Failure));
    return;
  }

  bool low_delay = true;
  ::media::CdmContext* cdm_context = nullptr;
  ::media::VideoDecoder::OutputCB output_cb =
      ConvertToBaseRepeatingCallback(CrossThreadBindRepeating(
          &DiscordVideoDecoderMediaThread::OnOutput, weak_this_));

  video_decoder_->Initialize(
      config_, low_delay, cdm_context,
      base::BindOnce(&DiscordVideoDecoderMediaThread::OnInitializeDone,
                     ConvertToBaseOnceCallback(std::move(init_cb))),
      output_cb, base::DoNothing());
}

void DiscordVideoDecoderMediaThread::OnInitializeDone(
    base::OnceCallback<void(ElectronVideoStatus)> cb,
    ::media::DecoderStatus status) {
  // TODO(eiz): translate this status into something more meaningful?
  std::move(cb).Run(status.is_ok() ? ElectronVideoStatus::Success
                                   : ElectronVideoStatus::Failure);
}

void DiscordVideoDecoderMediaThread::DecodeOnMediaThread() {
  DCHECK(media_task_runner_->RunsTasksInCurrentSequence());
  int max_decode_requests = video_decoder_->GetMaxDecodeRequests();

  while (outstanding_decode_requests_ < max_decode_requests) {
    scoped_refptr<::media::DecoderBuffer> buffer;

    {
      base::AutoLock auto_lock(lock_);

      if (pending_buffers_.empty()) {
        return;
      }

      buffer = pending_buffers_.front();
      pending_buffers_.pop_front();
    }

    outstanding_decode_requests_++;
    video_decoder_->Decode(
        std::move(buffer),
        base::BindOnce(&DiscordVideoDecoderMediaThread::OnDecodeDone,
                       weak_this_));
  }
}

void DiscordVideoDecoderMediaThread::OnDecodeDone(
    ::media::DecoderStatus status) {
  DCHECK(media_task_runner_->RunsTasksInCurrentSequence());
  outstanding_decode_requests_--;

  if (!status.is_ok() &&
      status.code() != ::media::DecoderStatus::Codes::kAborted) {
    base::AutoLock auto_lock(lock_);

    if (!first_error_.has_value()) {
      first_error_ = {status.message()};
    }
    pending_buffers_.clear();
    return;
  }

  DecodeOnMediaThread();
}

void DiscordVideoDecoderMediaThread::OnOutput(
    scoped_refptr<::media::VideoFrame> frame) {
  DCHECK(media_task_runner_->RunsTasksInCurrentSequence());
  ElectronPointer<IElectronVideoFrame> wrapped_frame =
      new DiscordVideoFrame(frame);

  base::AutoLock auto_lock(lock_);
  if (video_sink_) {
    video_sink_(&*wrapped_frame, user_data_);
  }
}

ElectronVideoStatus DiscordVideoDecoderMediaThread::SubmitBuffer(
    IElectronBuffer* buffer,
    uint32_t timestamp,
    ElectronVideoDecodeErrorCB* errorCB,
    void* userData) {
  scoped_refptr<::media::DecoderBuffer> decoder_buffer =
      ::media::DecoderBuffer::CopyFrom(
          base::span(buffer->GetBytes(), buffer->GetLength()));
  decoder_buffer->set_timestamp(base::Microseconds(timestamp));

  {
    base::AutoLock auto_lock(lock_);

    if (first_error_.has_value()) {
      if (errorCB != nullptr) {
        errorCB(first_error_->data(), first_error_->size(), userData);
      }
      return ElectronVideoStatus::Failure;
    }

    pending_buffers_.push_back(std::move(decoder_buffer));
  }

  blink::PostCrossThreadTask(
      *media_task_runner_.get(), FROM_HERE,
      CrossThreadBindOnce(&DiscordVideoDecoderMediaThread::DecodeOnMediaThread,
                          weak_this_));
  return ElectronVideoStatus::Success;
}

DiscordVideoDecoder::DiscordVideoDecoder() {
  //
}

DiscordVideoDecoder::~DiscordVideoDecoder() {
  if (media_thread_state_) {
    media_thread_state_->DeleteSoon();
  }
}

ElectronVideoStatus DiscordVideoDecoder::Initialize(
    IElectronVideoFormat* format,
    ElectronVideoSink* video_sink,
    void* user_data) {
#ifdef PRINTF_DEBUGGING_IS_COOL_LOL
  freopen("CONIN$", "r", stdin);
  freopen("CONOUT$", "w", stdout);
  freopen("CONOUT$", "w", stderr);
#endif

  if (started_initialize_) {
    return ElectronVideoStatus::InvalidState;
  }

  started_initialize_ = true;

  // Get gpu_factories and media_task_runner on the main thread before
  // constructing DiscordVideoDecoderMediaThread
  base::ScopedAllowBaseSyncPrimitivesForTesting allow_wait;
  auto waiter =
      base::MakeRefCounted<WaitableSharedObject<GpuFactoriesAndTaskRunner>>();
  CrossThreadOnceFunction<void(GpuFactoriesAndTaskRunner)> factories_cb =
      CrossThreadBindOnce(
          &WaitableSharedObject<GpuFactoriesAndTaskRunner>::FinishWait, waiter);

  if (!blink::PostCrossThreadTask(
          *content::RenderThreadImpl::DeprecatedGetMainTaskRunner().get(),
          FROM_HERE,
          CrossThreadBindOnce(GetGpuFactoriesOnRenderThread,
                              std::move(factories_cb)))) {
    return ElectronVideoStatus::Failure;
  }
  std::optional<GpuFactoriesAndTaskRunner> result = waiter->Wait();

  if (!result.has_value() || !result->gpu_factories ||
      !result->media_task_runner) {
    return ElectronVideoStatus::Unsupported;
  }

  ::media::VideoDecoderConfig config(
      static_cast<::media::VideoCodec>(format->GetCodec()),
      static_cast<::media::VideoCodecProfile>(format->GetProfile()),
      ::media::VideoDecoderConfig::AlphaMode::kIsOpaque,
      ::media::VideoColorSpace(), ::media::kNoTransformation, kDefaultSize,
      gfx::Rect(kDefaultSize), kDefaultSize, ::media::EmptyExtraData(),
      ::media::EncryptionScheme::kUnencrypted);

  if (result->gpu_factories->IsDecoderConfigSupported(config) ==
      ::media::GpuVideoAcceleratorFactories::Supported::kFalse) {
    return ElectronVideoStatus::Unsupported;
  }

  media_thread_state_ = new DiscordVideoDecoderMediaThread(
      std::move(config), video_sink, user_data, result->gpu_factories,
      result->media_task_runner);

  ELECTRON_VIDEO_RETURN_IF_ERR(media_thread_state_->Initialize());
  initialized_ = true;
  return ElectronVideoStatus::Success;
}

ElectronVideoStatus DiscordVideoDecoder::SubmitBuffer(
    IElectronBuffer* buffer,
    uint32_t timestamp,
    ElectronVideoDecodeErrorCB* errorCB,
    void* userData) {
  if (!initialized_) {
    return ElectronVideoStatus::InvalidState;
  }

  return media_thread_state_->SubmitBuffer(buffer, timestamp, errorCB,
                                           userData);
}

}  // namespace electron
}  // namespace media
}  // namespace discord
