#include "electron/discord/public/discord_video_frame.h"

#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/renderer/platform/webrtc/convert_to_webrtc_video_frame_buffer.h"

#include "content/renderer/renderer_blink_platform_impl.h"

#include "media/base/video_types.h"
#include "media/base/video_util.h"

namespace discord {
namespace media {
namespace electron {

constexpr char IElectronVideoFrameMedia::IID[];

DiscordVideoFormat::DiscordVideoFormat() {}
DiscordVideoFormat::~DiscordVideoFormat() {}

ElectronVideoStatus DiscordVideoFormat::SetCodec(ElectronVideoCodec codec) {
  if (static_cast<int>(codec) > static_cast<int>(kVideoCodecMax)) {
    return ElectronVideoStatus::Failure;
  }

  codec_ = codec;
  return ElectronVideoStatus::Success;
}

ElectronVideoCodec DiscordVideoFormat::GetCodec() {
  return codec_;
}

ElectronVideoStatus DiscordVideoFormat::SetProfile(
    ElectronVideoCodecProfile profile) {
  if (static_cast<int>(profile) > static_cast<int>(VIDEO_CODEC_PROFILE_MAX)) {
    return ElectronVideoStatus::Failure;
  }

  profile_ = profile;
  return ElectronVideoStatus::Success;
}

ElectronVideoCodecProfile DiscordVideoFormat::GetProfile() {
  return profile_;
}

DiscordVideoFrame::DiscordVideoFrame(scoped_refptr<::media::VideoFrame> frame)
    : frame_(frame) {}
DiscordVideoFrame::~DiscordVideoFrame() {}

uint32_t DiscordVideoFrame::GetWidth() {
  return frame_->visible_rect().width();
}

uint32_t DiscordVideoFrame::GetHeight() {
  return frame_->visible_rect().height();
}

uint32_t DiscordVideoFrame::GetTimestamp() {
  return static_cast<uint32_t>(frame_->timestamp().InMicroseconds());
}

class RtcI420FrameWrapper
    : public ElectronObject<IElectronVideoFrame, IElectronVideoFrameData> {
 public:
  explicit RtcI420FrameWrapper(
      rtc::scoped_refptr<webrtc::I420BufferInterface> frame)
      : frame_(frame) {}
  ~RtcI420FrameWrapper() override {}

  // IElectronVideoFrame
  uint32_t GetWidth() override { return frame_->width(); }
  uint32_t GetHeight() override { return frame_->height(); }
  IElectronVideoFrameData* ToI420() override { return this; }

  // Uncalled
  uint32_t GetTimestamp() override { return UINT32_MAX; }

  // IElectronVideoFrameData
  bool IsMappable() override { return true; }
  bool HasTextures() override { return false; }
  bool HasMappableGpuBuffer() override { return false; }
  ElectronVideoPixelFormat GetFormat() override { return PIXEL_FORMAT_I420; }
  ElectronVideoStorageType GetStorageType() override {
    return STORAGE_OWNED_MEMORY;
  }
  int GetStride(size_t plane) override {
    switch (plane) {
      case 0:
        return frame_->StrideY();
      case 1:
        return frame_->StrideU();
      case 2:
        return frame_->StrideV();
    }
    return 0;
  }
  uint8_t const* GetData(size_t plane) override {
    switch (plane) {
      case 0:
        return frame_->DataY();
      case 1:
        return frame_->DataU();
      case 2:
        return frame_->DataV();
    }
    return nullptr;
  }

  // These are uncalled
  int GetRowBytes(size_t plane) override {
    switch (plane) {
      case 0:
        return GetWidth();
      case 1:
      case 2:
        return GetWidth() / 2;
    }
    return 0;
  }
  int GetRows(size_t plane) override {
    switch (plane) {
      case 0:
        return GetHeight();
      case 1:
      case 2:
        return GetHeight() / 2;
    }
    return 0;
  }

 private:
  rtc::scoped_refptr<webrtc::I420BufferInterface> frame_;
};

IElectronVideoFrameData* DiscordVideoFrame::ToI420() {
  if (!frame_)
    return nullptr;

  if (!::blink::CanConvertToWebRtcVideoFrameBuffer(frame_.get()))
    return nullptr;

  auto shared =
      base::MakeRefCounted<::blink::WebRtcVideoFrameAdapter::SharedResources>(
          ::blink::Platform::Current()->GetGpuFactories());

  auto ret = ConvertToWebRtcVideoFrameBuffer(frame_, shared);

  if (!ret)
    return nullptr;

  return new RtcI420FrameWrapper(ret->ToI420());
}

::media::VideoFrame* DiscordVideoFrame::GetMediaFrame() {
  return frame_.get();
}

bool DiscordVideoFrame::IsMappable() {
  return frame_->IsMappable();
}

bool DiscordVideoFrame::HasTextures() {
  return frame_->HasTextures();
}

bool DiscordVideoFrame::HasMappableGpuBuffer() {
  return frame_->HasMappableGpuBuffer();
}

ElectronVideoPixelFormat DiscordVideoFrame::GetFormat() {
  return static_cast<ElectronVideoPixelFormat>(frame_->format());
}

ElectronVideoStorageType DiscordVideoFrame::GetStorageType() {
  return static_cast<ElectronVideoStorageType>(frame_->storage_type());
}

int DiscordVideoFrame::GetStride(size_t plane) {
  return frame_->stride(plane);
}

int DiscordVideoFrame::GetRowBytes(size_t plane) {
  return frame_->row_bytes(plane);
}

int DiscordVideoFrame::GetRows(size_t plane) {
  return frame_->rows(plane);
}

uint8_t const* DiscordVideoFrame::GetData(size_t plane) {
  return frame_->visible_data(plane);
}

void DiscordVideoFrame::PrintDebugLog() {
  if (!frame_) {
    fprintf(stderr, "Null frame!\n");
    return;
  }

  fprintf(stderr, "Video frame %p\n", frame_.get());
  fprintf(stderr, "IsMappable %s\n", frame_->IsMappable() ? "true" : "false");
  fprintf(stderr, "HasTextures %s\n", frame_->HasTextures() ? "true" : "false");
  fprintf(stderr, "NumTextures %zu\n", frame_->NumTextures());
  fprintf(stderr, "HasMappableGpuBuffer %s\n",
          frame_->HasMappableGpuBuffer() ? "true" : "false");
  fprintf(stderr, "Format %d\n", (int)frame_->format());
  fprintf(stderr, "Storage type %d\n", (int)frame_->storage_type());
  fprintf(stderr, "Width %lu\n", (unsigned long)GetWidth());
  fprintf(stderr, "Height %lu\n", (unsigned long)GetHeight());
  fprintf(stderr, "Planes %zu\n", frame_->NumPlanes(frame_->format()));
}
}  // namespace electron
}  // namespace media
}  // namespace discord
