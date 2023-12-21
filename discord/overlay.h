#include <cstddef>
#include <cstdint>

// Support for Electron debug builds that enable checks for rawptr.
// Use our own definition to avoid conflicts with Chromium's PA
// RAW_PTR_EXCLUSION macro
#define DISCORD_RAW_PTR_EXCLUSION __attribute__((annotate("raw_ptr_exclusion")))

namespace discord {

class Overlay {
 public:
  void SetProcessId(uint32_t process_id);
  bool SendFrame(uint32_t width, uint32_t height, void* data, size_t length);

 private:
  uint32_t process_id_ = 0;

  bool send_function_loaded_ = false;
  DISCORD_RAW_PTR_EXCLUSION const void* send_function_ = nullptr;
};

}  // namespace discord
