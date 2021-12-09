#include "electron/discord/discord_video_sink_manager.h"

#include "third_party/blink/renderer/modules/discord_video/discord_video.h"

namespace discord {
namespace media {
namespace electron {
DiscordVideoSinkManager::DiscordVideoSinkManager() {}
DiscordVideoSinkManager::~DiscordVideoSinkManager() {}

ElectronVideoStatus DiscordVideoSinkManager::SetAttachment(
    char const* sinkId,
    IElectronVideoSinkAttachment* attachment) {
  if (!blink::DiscordVideoSink::SetAttachment(sinkId, attachment)) {
    return ElectronVideoStatus::Failure;
  }

  return ElectronVideoStatus::Success;
}
}  // namespace electron
}  // namespace media
}  // namespace discord