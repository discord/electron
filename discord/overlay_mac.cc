#include "discord/overlay.h"

namespace discord {

bool DiscordOverlay::SendFrame(uint32_t width,
                               uint32_t height,
                               void* data,
                               size_t length) {
  return false;
}

void DiscordOverlay::SetProcessId(uint32_t process_id) {}

}  // namespace discord
