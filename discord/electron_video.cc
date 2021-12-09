#define ELECTRON_VIDEO_IMPLEMENTATION

#include <vector>

#include "electron/discord/discord_video_decoder.h"
#include "electron/discord/discord_video_sink_manager.h"
#include "electron/discord/public/discord_video_frame.h"
#include "electron/discord/public/electron_video_shared.h"

namespace discord {
namespace media {
namespace electron {

ElectronVideoStatus ElectronVideoCreateObject(char const* clsid,
                                              char const* iid,
                                              void** ppVideoObject) {
  ElectronPointer<IElectronUnknown> ptr;

  if (!strcmp(clsid, "DiscordVideoDecoder")) {
    ptr = new DiscordVideoDecoder();
  } else if (!strcmp(clsid, "DiscordVideoFormat")) {
    ptr = new DiscordVideoFormat();
  } else if (!strcmp(clsid, "DiscordVideoSinkManager")) {
    ptr = new DiscordVideoSinkManager();
  }

  if (!ptr) {
    return ElectronVideoStatus::ClassNotFound;
  }

  return ptr->QueryInterface(iid, ppVideoObject);
}

}  // namespace electron
}  // namespace media
}  // namespace discord
