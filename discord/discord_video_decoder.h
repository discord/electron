#pragma once

#include "base/memory/scoped_refptr.h"
#include "discord/electron_video_shared.h"
#include "media/base/video_decoder.h"

namespace base {
class SingleThreadTaskRunner;
} // namespace base

namespace media {
class DecoderBuffer;
class GpuVideoAcceleratorFactories;
class MediaLog;
class VideoFrame;
}  // namespace media

namespace discord {
namespace media {
namespace electron {

class DiscordVideoDecoder : public ElectronObject<IElectronVideoDecoder> {
 public:
  DiscordVideoDecoder();
  ~DiscordVideoDecoder() override;
  ElectronVideoStatus Initialize(IElectronVideoFormat* format,
                                 ElectronVideoSink* videoSink) override;
  ElectronVideoStatus SubmitBuffer(IElectronBuffer* buffer,
                                   void* userData) override;

 private:
  scoped_refptr<base::SingleThreadTaskRunner> media_task_runner_;
  std::unique_ptr<::media::MediaLog> media_log_;
  std::unique_ptr<::media::VideoDecoder> video_decoder_;
};

}  // namespace electron
}  // namespace media
}  // namespace discord