#include "discord/overlay.h"

namespace discord {

bool Overlay::SendFrame(
                 const gfx::Size& ,
                 const gfx::Rect& ,
                 base::UnsafeSharedMemoryRegion ,
                 base::ReadOnlySharedMemoryRegion ,
                 base::OnceCallback<void()> ) {
  (void)process_id_;
  (void)send_function_loaded_;
  (void)send_function_;
  return false;
}

void Overlay::SetProcessId(uint32_t) {}

}  // namespace discord
