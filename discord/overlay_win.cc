#include "discord/overlay.h"

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
  const static uint32_t Version = 1;

  uint32_t process_id;
  uint32_t width;
  uint32_t height;
  DISCORD_RAW_PTR_EXCLUSION void* data;
  size_t length;
};

bool Overlay::SendFrame(uint32_t width,
                        uint32_t height,
                        void* data,
                        size_t length) {
  if (process_id_ == 0) {
    return false;
  } else {
    auto frame = OverlayFrameData{process_id_, width, height, data, length};

    auto send_frame =
        FindOverlayFunction<bool (*)(uint32_t version, void* data)>(
            send_function_loaded_, send_function_, "SendOverlayFrame");

    return send_frame != nullptr &&
           send_frame(OverlayFrameData::Version, &frame);
  }
}

void Overlay::SetProcessId(uint32_t process_id) {
  process_id_ = process_id;
}

}  // namespace discord
