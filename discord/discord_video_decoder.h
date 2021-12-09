#pragma once

#include "base/memory/scoped_refptr.h"
#include "electron/discord/public/discord_video_frame.h"
#include "electron/discord/public/electron_video_shared.h"
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
                                 ElectronVideoSink* videoSink,
                                 void* userData) override;
  ElectronVideoStatus SubmitBuffer(IElectronBuffer* buffer,
                                   uint32_t timestamp) override;

 private:
  DiscordVideoDecoderMediaThread* media_thread_state_{};
  bool started_initialize_{false};
  bool initialized_{false};
};

}  // namespace electron
}  // namespace media
}  // namespace discord
