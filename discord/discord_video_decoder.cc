#include "discord/discord_video_decoder.h"

#include "base/single_thread_task_runner.h"
#include "media/base/media_log.h"
#include "media/base/video_types.h"

namespace discord {
namespace media {
namespace electron {

DiscordVideoDecoder::DiscordVideoDecoder() {
  //
}

DiscordVideoDecoder::~DiscordVideoDecoder() {
  //
}

ElectronVideoStatus DiscordVideoDecoder::Initialize(
    IElectronVideoFormat* format,
    ElectronVideoSink* videoSink) {
  return ElectronVideoStatus::Failure;
}

ElectronVideoStatus DiscordVideoDecoder::SubmitBuffer(IElectronBuffer* buffer,
                                                      void* userData) {
  return ElectronVideoStatus::Failure;
}

}  // namespace electron
}  // namespace media
}  // namespace discord