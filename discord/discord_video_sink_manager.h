#pragma once

#include "electron/discord/public/electron_video_shared.h"

namespace discord {
namespace media {
namespace electron {
class DiscordVideoSinkManager
    : public ElectronObject<IElectronVideoSinkManager> {
 public:
  DiscordVideoSinkManager();
  ~DiscordVideoSinkManager() override;
  ElectronVideoStatus SetAttachment(
      char const* sinkId,
      IElectronVideoSinkAttachment* attachment) override;
};
}  // namespace electron
}  // namespace media
}  // namespace discord