#include "discord/overlay.h"

namespace discord {

bool Overlay::SendFrame(uint32_t, uint32_t, void*, size_t) {
  (void)process_id_;
  (void)send_function_loaded_;
  (void)send_function_;
  return false;
}

void Overlay::SetProcessId(uint32_t) {}

}  // namespace discord
