#include "discord/discord_video_decoder.h"

#include "base/single_thread_task_runner.h"
#include "media/base/media_log.h"
#include "media/base/media_util.h"
#include "media/base/video_decoder_config.h"
#include "media/base/video_types.h"
#include "media/video/gpu_video_accelerator_factories.h"
#include "third_party/blink/public/platform/platform.h"

namespace discord {
namespace media {
namespace electron {

namespace {
// Arbitrary, matches RTCVideoDecoderAdapter.
const gfx::Size kDefaultSize(640, 480);
}  // namespace

class DiscordVideoDecoderMediaThread {
 public:
  DiscordVideoDecoderMediaThread(
      ::media::GpuVideoAcceleratorFactories* gpu_factories,
      ::media::VideoDecoderConfig const& config,
      ElectronVideoSink* videoSink);
  ElectronVideoStatus Initialize();
  ElectronVideoStatus SubmitBuffer(IElectronBuffer* buffer, void* userData);

 private:
  std::unique_ptr<::media::MediaLog> media_log_;
  std::unique_ptr<::media::VideoDecoder> video_decoder_;
};

DiscordVideoDecoderMediaThread::DiscordVideoDecoderMediaThread(
    ::media::GpuVideoAcceleratorFactories* gpu_factories,
    ::media::VideoDecoderConfig const& config,
    ElectronVideoSink* videoSink) {}
ElectronVideoStatus DiscordVideoDecoderMediaThread::Initialize() {
  return ElectronVideoStatus::Failure;
}
ElectronVideoStatus DiscordVideoDecoderMediaThread::SubmitBuffer(
    IElectronBuffer* buffer,
    void* userData) {
  return ElectronVideoStatus::Failure;
}

DiscordVideoDecoder::DiscordVideoDecoder() {
  //
}

DiscordVideoDecoder::~DiscordVideoDecoder() {
  if (media_thread_state_) {
    gpu_factories_->GetTaskRunner()->DeleteSoon(FROM_HERE, media_thread_state_);
  }
}

ElectronVideoStatus DiscordVideoDecoder::Initialize(
    IElectronVideoFormat* format,
    ElectronVideoSink* videoSink) {
  ElectronVideoCodec codec;
  ElectronVideoCodecProfile profile;

  if (gpu_factories_) {
    // Can't reinitialize. Make a new one.
    return ElectronVideoStatus::InvalidState;
  }

  ELECTRON_VIDEO_RETURN_IF_ERR(format->GetCodec(&codec));
  ELECTRON_VIDEO_RETURN_IF_ERR(format->GetProfile(&profile));

  gpu_factories_ = blink::Platform::Current()->GetGpuFactories();

  ::media::VideoDecoderConfig config(
      static_cast<::media::VideoCodec>(codec),
      static_cast<::media::VideoCodecProfile>(profile),
      ::media::VideoDecoderConfig::AlphaMode::kIsOpaque,
      ::media::VideoColorSpace(), ::media::kNoTransformation, kDefaultSize,
      gfx::Rect(kDefaultSize), kDefaultSize, ::media::EmptyExtraData(),
      ::media::EncryptionScheme::kUnencrypted);

  if (gpu_factories_->IsDecoderConfigSupported(
          ::media::VideoDecoderImplementation::kDefault, config) ==
      ::media::GpuVideoAcceleratorFactories::Supported::kFalse) {
    return ElectronVideoStatus::Unsupported;
  }

  media_thread_state_ =
      new DiscordVideoDecoderMediaThread(gpu_factories_, config, videoSink);

  ELECTRON_VIDEO_RETURN_IF_ERR(media_thread_state_->Initialize());
  initialized_ = true;
  return ElectronVideoStatus::Success;
}

ElectronVideoStatus DiscordVideoDecoder::SubmitBuffer(IElectronBuffer* buffer,
                                                      void* userData) {
  if (!initialized_) {
    return ElectronVideoStatus::InvalidState;
  }

  return media_thread_state_->SubmitBuffer(buffer, userData);
}

}  // namespace electron
}  // namespace media
}  // namespace discord