#define ELECTRON_VIDEO_IMPLEMENTATION

#include <vector>

#include "discord/discord_video_decoder.h"
#include "discord/electron_video_shared.h"

namespace discord {
namespace media {
namespace electron {

class DiscordBuffer : public ElectronObject<IElectronBuffer> {
 public:
  DiscordBuffer(size_t length) { buffer_.resize(length); }

  uint8_t* GetBytes() override { return buffer_.data(); }
  size_t GetLength() override { return buffer_.size(); }

 private:
  std::vector<uint8_t> buffer_{};
};

class DiscordBufferPool : public ElectronObject<IElectronBufferPool> {
 public:
  DiscordBufferPool() {}
  ElectronVideoStatus Initialize(size_t length, size_t count) override {
    length_ = length;
    return ElectronVideoStatus::Success;
  }
  ElectronVideoStatus CreateBuffer(IElectronBuffer** buffer) override {
    *buffer = new DiscordBuffer(length_);
    return ElectronVideoStatus::Success;
  }

 private:
  size_t length_{};
};

ElectronVideoStatus ElectronVideoCreateObject(char const* clsid,
                                              char const* iid,
                                              void** ppVideoObject) {
  ElectronPointer<IElectronUnknown> ptr;

  if (!strcmp(clsid, "DiscordBufferPool")) {
    ptr = new DiscordBufferPool();
  }

  if (!strcmp(clsid, "DiscordVideoDecoder")) {
    ptr = new DiscordVideoDecoder();
  }

  if (!ptr) {
    return ElectronVideoStatus::ClassNotFound;
  }

  return ptr->QueryInterface(iid, ppVideoObject);
}
}  // namespace electron
}  // namespace media
}  // namespace discord