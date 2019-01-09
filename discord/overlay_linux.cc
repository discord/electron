#include "discord/overlay.h"

namespace discord {

bool Overlay::SendFrame(uint32_t width,
                        uint32_t height,
                        void* data,
                        size_t length) {
  return false;
}

void Overlay::SetProcessId(uint32_t process_id) {}

}  // namespace discord
