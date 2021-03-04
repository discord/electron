#pragma once

#include "base/memory/scoped_refptr.h"
#include "discord/electron_video_shared.h"
#include "media/base/video_decoder.h"

namespace base {
class SingleThreadTaskRunner;
}  // namespace base

namespace media {
class DecoderBuffer;
class GpuVideoAcceleratorFactories;
class MediaLog;
class VideoFrame;
}  // namespace media

namespace discord {
namespace media {
namespace electron {

class DiscordVideoDecoderMediaThread;

class DiscordVideoDecoder : public ElectronObject<IElectronVideoDecoder> {
 public:
  DiscordVideoDecoder();
  ~DiscordVideoDecoder() override;
  ElectronVideoStatus Initialize(IElectronVideoFormat* format,
                                 ElectronVideoSink* videoSink) override;
  ElectronVideoStatus SubmitBuffer(IElectronBuffer* buffer,
                                   uint32_t timestamp) override;

 private:
  ::media::GpuVideoAcceleratorFactories* gpu_factories_;
  DiscordVideoDecoderMediaThread* media_thread_state_{};
  bool initialized_{false};
};

class IElectronVideoFramePrivate : IElectronVideoFrame {
 public:
  static constexpr char IID[] = "IElectronVideoFramePrivate";
  virtual ElectronVideoStatus GetMediaFrame(
      ::media::VideoFrame** ppMediaFrame) = 0;
};

}  // namespace electron
}  // namespace media
}  // namespace discord