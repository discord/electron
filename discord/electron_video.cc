#define ELECTRON_VIDEO_IMPLEMENTATION
#define ELECTRON_VIDEO_DECLARE_IIDS

#include <vector>

#include "discord/discord_video_decoder.h"
#include "discord/electron_video_shared.h"

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
  }

  if (!ptr) {
    return ElectronVideoStatus::ClassNotFound;
  }

  return ptr->QueryInterface(iid, ppVideoObject);
}

}  // namespace electron
}  // namespace media
}  // namespace discord
