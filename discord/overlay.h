#include <cstddef>
#include <cstdint>

#include "base/callback.h"
#include "base/memory/unsafe_shared_memory_region.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace discord {

class Overlay {
 public:
  void SetProcessId(uint32_t process_id);
  void SendFrame(const gfx::Size& pixel_size,
                 const gfx::Rect& damage_rect,
                 base::UnsafeSharedMemoryRegion unsafe_data,
                 base::ReadOnlySharedMemoryRegion read_only_data,
                 base::OnceCallback<void()> callback);

 private:
  uint32_t process_id_ = 0;

  bool send_function_loaded_ = false;
  const void* send_function_ = nullptr;
};

}  // namespace discord
