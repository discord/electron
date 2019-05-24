#include "discord/overlay.h"
#include "base/task/post_task.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"

#include <windows.h>

namespace {

// attempts to load `function_name` from `discord_overlay2.node`.
//
// # remarks.
//
// we use `loaded` and `function` as storage to cache the requested function.
// once `discord_overlay2.node` is available, we will only try to resolve
// `function_name` once, then cache its result. so invocations of
// `FindOverlayFunction()` are relatively cheap.
template <typename TFunction>
TFunction FindOverlayFunction(bool& loaded,
                              const void*& function,
                              const char* function_name) {
  constexpr auto* kModuleName = L"discord_overlay2.node";

  HMODULE module_handle;

  if (!loaded && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, kModuleName,
                                    &module_handle) != 0) {
    loaded = true;
    function = reinterpret_cast<const void*>(
        GetProcAddress(module_handle, function_name));
  }

  return (TFunction)function;
}

}  // namespace

namespace discord {

struct OverlayFrameData {
  const static uint32_t Version = 2;

  uint32_t process_id;
  uint32_t width;
  uint32_t height;
  HANDLE data;
  uint32_t data_size;
  uint32_t damage_x;
  uint32_t damage_y;
  uint32_t damage_w;
  uint32_t damage_h;
};

void Overlay::SendFrame(const gfx::Size& pixel_size,
                        const gfx::Rect& damage_rect,
                        base::UnsafeSharedMemoryRegion unsafe_data,
                        base::ReadOnlySharedMemoryRegion read_only_data,
                        base::OnceCallback<void()> callback) {
  HANDLE handle;
  uint32_t region_size;
  if (unsafe_data.IsValid()) {
    auto region = base::UnsafeSharedMemoryRegion::TakeHandleForSerialization(
        std::move(unsafe_data));
    region_size = region.GetSize();
    auto scoped = region.PassPlatformHandle();
    handle = scoped.Take();
  }
  else if (read_only_data.IsValid()) {
    auto region = base::ReadOnlySharedMemoryRegion::TakeHandleForSerialization(
        std::move(read_only_data));
    region_size = region.GetSize();
    auto scoped = region.PassPlatformHandle();
    handle = scoped.Take();
  }
  else {
    std::move(callback).Run();
    return;
  }

  OverlayFrameData frame;
  frame.process_id = process_id_;
  frame.width = pixel_size.width();
  frame.height = pixel_size.height();
  frame.data = handle;
  frame.data_size = (uint32_t)region_size;
  frame.damage_x = damage_rect.origin().x();
  frame.damage_y = damage_rect.origin().y();
  frame.damage_w = damage_rect.width();
  frame.damage_h = damage_rect.height();

  auto send_frame = FindOverlayFunction<bool (*)(uint32_t version, void* data)>(
      send_function_loaded_, send_function_, "SendOverlayFrame");

  if (send_frame != nullptr) {
    send_frame(OverlayFrameData::Version, &frame);
  }

  std::move(callback).Run();
}

void Overlay::SetProcessId(uint32_t process_id) {
  process_id_ = process_id;
}

}  // namespace discord
