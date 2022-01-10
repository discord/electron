#include <cstddef>
#include <cstdint>

namespace discord {

class Overlay {
 public:
  void SetProcessId(uint32_t process_id);
  bool SendFrame(uint32_t width, uint32_t height, void* data, size_t length);

 private:
  uint32_t process_id_ = 0;

  bool send_function_loaded_ = false;
  const void* send_function_ = nullptr;
};

}  // namespace discord
