#include "discord/discord_video_decoder.h"

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

namespace WTF {

template <>
struct CrossThreadCopier<::media::VideoDecoderConfig>
    : public CrossThreadCopierPassThrough<::media::VideoDecoderConfig> {
  STATIC_ONLY(CrossThreadCopier);
};

}  // namespace WTF

namespace discord {
namespace media {
namespace electron {

constexpr char IElectronVideoFrameMedia::IID[];

namespace {
using InitCB = CrossThreadOnceFunction<void(ElectronVideoStatus)>;

// Arbitrary, matches RTCVideoDecoderAdapter.
const gfx::Size kDefaultSize(640, 480);

void FinishWait(base::WaitableEvent* waiter,
                ElectronVideoStatus* result_out,
                ElectronVideoStatus result) {
  DVLOG(3) << __func__ << "(" << static_cast<int>(result) << ")";
  *result_out = result;
  waiter->Signal();
}

void OnRequestOverlayInfo(bool decoder_requires_restart_for_overlay,
                          ::media::ProvideOverlayInfoCB overlay_info_cb) {
  // Android overlays are not supported.
  if (overlay_info_cb)
    std::move(overlay_info_cb).Run(::media::OverlayInfo());
}

}  // namespace

DiscordVideoFormat::DiscordVideoFormat() {}
DiscordVideoFormat::~DiscordVideoFormat() {}

ElectronVideoStatus DiscordVideoFormat::SetCodec(ElectronVideoCodec codec) {
  if (static_cast<int>(codec) > static_cast<int>(kVideoCodecMax)) {
    return ElectronVideoStatus::Failure;
  }

  codec_ = codec;
  return ElectronVideoStatus::Success;
}

ElectronVideoCodec DiscordVideoFormat::GetCodec() {
  return codec_;
}

ElectronVideoStatus DiscordVideoFormat::SetProfile(
    ElectronVideoCodecProfile profile) {
  if (static_cast<int>(profile) > static_cast<int>(VIDEO_CODEC_PROFILE_MAX)) {
    return ElectronVideoStatus::Failure;
  }

  profile_ = profile;
  return ElectronVideoStatus::Success;
}

ElectronVideoCodecProfile DiscordVideoFormat::GetProfile() {
  return profile_;
}

class DiscordVideoFrame
    : public ElectronObject<IElectronVideoFrameMedia, IElectronVideoFrame> {
 public:
  DiscordVideoFrame(scoped_refptr<::media::VideoFrame> frame);
  uint32_t GetWidth() override;
  uint32_t GetHeight() override;
  uint32_t GetTimestamp() override;
  ElectronVideoStatus ToI420(IElectronBuffer* outputBuffer) override;
  ::media::VideoFrame* GetMediaFrame() override;

 private:
  scoped_refptr<::media::VideoFrame> frame_;
};

DiscordVideoFrame::DiscordVideoFrame(scoped_refptr<::media::VideoFrame> frame)
    : frame_(frame) {}

uint32_t DiscordVideoFrame::GetWidth() {
  return frame_->coded_size().width();
}

uint32_t DiscordVideoFrame::GetHeight() {
  return frame_->coded_size().height();
}

uint32_t DiscordVideoFrame::GetTimestamp() {
  return static_cast<uint32_t>(frame_->timestamp().InMicroseconds());
}

ElectronVideoStatus DiscordVideoFrame::ToI420(IElectronBuffer* outputBuffer) {
  return ElectronVideoStatus::Failure;
}

::media::VideoFrame* DiscordVideoFrame::GetMediaFrame() {
  return frame_.get();
}

class DiscordVideoDecoderMediaThread {
 public:
  DiscordVideoDecoderMediaThread(IElectronVideoFormat* format,
                                 ElectronVideoSink* video_sink,
                                 void* user_data);

  DiscordVideoDecoderMediaThread(const DiscordVideoDecoderMediaThread&) =
      delete;
  DiscordVideoDecoderMediaThread& operator=(
      const DiscordVideoDecoderMediaThread&) = delete;

  ElectronVideoStatus Initialize();
  ElectronVideoStatus SubmitBuffer(IElectronBuffer* buffer, uint32_t timestamp);
  void DeleteSoon();

 private:
  void InitializeOnMainThread(InitCB init_cb);
  void InitializeOnMediaThread(const ::media::VideoDecoderConfig& config,
                               InitCB init_cb);
  static void OnInitializeDone(base::OnceCallback<void(ElectronVideoStatus)> cb,
                               ::media::DecoderStatus status);
  void DecodeOnMediaThread();
  void OnDecodeDone(::media::DecoderStatus status);
  void OnOutput(scoped_refptr<::media::VideoFrame> frame);

  ElectronPointer<IElectronVideoFormat> format_;
  void* user_data_;
  ::media::GpuVideoAcceleratorFactories* gpu_factories_{};
  std::unique_ptr<::media::MediaLog> media_log_;
  std::unique_ptr<::media::VideoDecoder> video_decoder_;
  scoped_refptr<base::SequencedTaskRunner> media_task_runner_;
  int32_t outstanding_decode_requests_{0};

  // shared state
  base::Lock lock_;
  bool has_error_{false};
  WTF::Deque<scoped_refptr<::media::DecoderBuffer>> pending_buffers_;
  ElectronVideoSink* video_sink_;
  base::WeakPtr<DiscordVideoDecoderMediaThread> weak_this_;
  base::WeakPtrFactory<DiscordVideoDecoderMediaThread> weak_this_factory_{this};
};

DiscordVideoDecoderMediaThread::DiscordVideoDecoderMediaThread(
    IElectronVideoFormat* format,
    ElectronVideoSink* video_sink,
    void* user_data)
    : format_(RetainElectronVideoObject(format)),
      user_data_(user_data),
      video_sink_(video_sink) {
  weak_this_ = weak_this_factory_.GetWeakPtr();
}

void DiscordVideoDecoderMediaThread::DeleteSoon() {
  {
    base::AutoLock auto_lock(lock_);
    pending_buffers_.clear();
    video_sink_ = nullptr;
  }
  if (gpu_factories_) {
    auto taskRunner = gpu_factories_->GetTaskRunner();
    if (taskRunner) {
      taskRunner->DeleteSoon(FROM_HERE, this);
      return;
    }
  }
  delete this;
}

ElectronVideoStatus DiscordVideoDecoderMediaThread::Initialize() {
  ElectronVideoStatus result = ElectronVideoStatus::Failure;

  // This is gross, but the non "ForTesting" version of this requires an
  // explicit friend class declaration, and I don't want to modify base just to
  // add us to the whitelist. It might be worth just moving the blocking wait
  // out of this code and into the consumer in Discord.
  base::ScopedAllowBaseSyncPrimitivesForTesting allow_wait;
  base::WaitableEvent waiter(base::WaitableEvent::ResetPolicy::MANUAL,
                             base::WaitableEvent::InitialState::NOT_SIGNALED);
  auto init_cb =
      CrossThreadBindOnce(&FinishWait, CrossThreadUnretained(&waiter),
                          CrossThreadUnretained(&result));

  if (blink::PostCrossThreadTask(
          // TODO(eiz): we have to do the main thread initialization a different
          // way as this global reference is not going to be around forever.
          *content::RenderThreadImpl::DeprecatedGetMainTaskRunner().get(),
          FROM_HERE,
          CrossThreadBindOnce(
              &DiscordVideoDecoderMediaThread::InitializeOnMainThread,
              CrossThreadUnretained(this), std::move(init_cb)))) {
    // TODO(crbug.com/1076817) Remove if a root cause is found.
    // TODO(eiz): ya, I dunno, I stole it from them.
    if (!waiter.TimedWait(base::Seconds(10)))
      return ElectronVideoStatus::TimedOut;
  }

  return result;
}

void DiscordVideoDecoderMediaThread::InitializeOnMainThread(InitCB init_cb) {
  gpu_factories_ = content::RenderThreadImpl::current()->GetGpuFactories();

  if (!gpu_factories_) {
    std::move(init_cb).Run(ElectronVideoStatus::Unsupported);
    return;
  }

  media_task_runner_ = gpu_factories_->GetTaskRunner();

  ::media::VideoDecoderConfig config(
      static_cast<::media::VideoCodec>(format_->GetCodec()),
      static_cast<::media::VideoCodecProfile>(format_->GetProfile()),
      ::media::VideoDecoderConfig::AlphaMode::kIsOpaque,
      ::media::VideoColorSpace(), ::media::kNoTransformation, kDefaultSize,
      gfx::Rect(kDefaultSize), kDefaultSize, ::media::EmptyExtraData(),
      ::media::EncryptionScheme::kUnencrypted);

  if (gpu_factories_->IsDecoderConfigSupported(config) ==
      ::media::GpuVideoAcceleratorFactories::Supported::kFalse) {
    blink::PostCrossThreadTask(
        *media_task_runner_.get(), FROM_HERE,
        CrossThreadBindOnce(std::move(init_cb),
                            ElectronVideoStatus::Unsupported));
    return;
  }

  blink::PostCrossThreadTask(
      *media_task_runner_.get(), FROM_HERE,
      CrossThreadBindOnce(
          &DiscordVideoDecoderMediaThread::InitializeOnMediaThread,
          CrossThreadUnretained(this), config, std::move(init_cb)));
}

void DiscordVideoDecoderMediaThread::InitializeOnMediaThread(
    const ::media::VideoDecoderConfig& config,
    InitCB init_cb) {
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
      config, low_delay, cdm_context,
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

    has_error_ = true;
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
    uint32_t timestamp) {
  scoped_refptr<::media::DecoderBuffer> decoder_buffer =
      ::media::DecoderBuffer::CopyFrom(
          base::span(buffer->GetBytes(), buffer->GetLength()));
  decoder_buffer->set_timestamp(base::Microseconds(timestamp));

  {
    base::AutoLock auto_lock(lock_);

    if (has_error_) {
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
  media_thread_state_ =
      new DiscordVideoDecoderMediaThread(format, video_sink, user_data);

  ELECTRON_VIDEO_RETURN_IF_ERR(media_thread_state_->Initialize());
  initialized_ = true;
  return ElectronVideoStatus::Success;
}

ElectronVideoStatus DiscordVideoDecoder::SubmitBuffer(IElectronBuffer* buffer,
                                                      uint32_t timestamp) {
  if (!initialized_) {
    return ElectronVideoStatus::InvalidState;
  }

  return media_thread_state_->SubmitBuffer(buffer, timestamp);
}

}  // namespace electron
}  // namespace media
}  // namespace discord
