#include "electron/discord/public/discord_video_frame.h"

#include <iostream>
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/renderer/platform/webrtc/convert_to_webrtc_video_frame_buffer.h"

#include "content/renderer/renderer_blink_platform_impl.h"

#include "base/no_destructor.h"
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
    : DiscordVideoFrame(std::move(frame), nullptr) {}
DiscordVideoFrame::DiscordVideoFrame(
    scoped_refptr<::media::VideoFrame> frame,
    ::media::GpuVideoAcceleratorFactories* gpu_factories)
    : frame_(std::move(frame)),
      gpu_factories_(gpu_factories) {}
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
      webrtc::scoped_refptr<webrtc::I420BufferInterface> frame)
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
  bool HasSharedImage() override { return false; }
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
  webrtc::scoped_refptr<webrtc::I420BufferInterface> frame_;
};

namespace {

scoped_refptr<::blink::WebRtcVideoFrameAdapter::SharedResources>
GetSharedResourcesSingleton(::media::GpuVideoAcceleratorFactories* gpu_factories) {
  static base::NoDestructor<
      scoped_refptr<::blink::WebRtcVideoFrameAdapter::SharedResources>>
      instance(
          ::blink::WebRtcVideoFrameAdapter::SharedResources::Create(
              gpu_factories));
  return *instance;
}

}  // namespace

IElectronVideoFrameData* DiscordVideoFrame::ToI420() {
  if (!frame_ || !gpu_factories_)
    return nullptr;

  if (!::blink::CanConvertToWebRtcVideoFrameBuffer(frame_.get()))
    return nullptr;

  auto shared = GetSharedResourcesSingleton(gpu_factories_);
  if (!shared)
    return nullptr;

  // ConvertToWebRtcVideoFrameBuffer needs a CPU accessible frame,
  // so try to get one
  scoped_refptr<::media::VideoFrame> cpu_frame;
  if (frame_->HasMappableSharedImage()) {
    cpu_frame = shared->ConstructVideoFrameFromGpu(frame_);
  } else if (frame_->HasSharedImage()) {
    cpu_frame = shared->ConstructVideoFrameFromTexture(frame_);
  } else {
    cpu_frame = frame_;  // already CPU accessible
  }
  if (!cpu_frame)
    return nullptr;

  auto converted = ConvertToWebRtcVideoFrameBuffer(std::move(cpu_frame), shared);
  if (!converted)
    return nullptr;

  return new RtcI420FrameWrapper(converted->ToI420());
}

::media::VideoFrame* DiscordVideoFrame::GetMediaFrame() {
  return frame_.get();
}

bool DiscordVideoFrame::IsMappable() {
  return frame_->HasDirectCpuAccess();
}

bool DiscordVideoFrame::HasSharedImage() {
  return frame_->HasSharedImage();
}

bool DiscordVideoFrame::HasMappableGpuBuffer() {
  return frame_->HasMappableSharedImage();
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
    std::cerr << "Null frame!" << std::endl;
    return;
  }

  std::cerr << "Video frame " << frame_.get() << std::endl;
  std::cerr << "IsMappable " << (frame_->HasDirectCpuAccess() ? "true" : "false")
            << std::endl;
  std::cerr << "HasSharedImage "
            << (frame_->HasSharedImage() ? "true" : "false") << std::endl;
  std::cerr << "HasMappableGpuBuffer "
            << (frame_->HasMappableSharedImage() ? "true" : "false")
            << std::endl;
  std::cerr << "Format " << (int)frame_->format() << std::endl;
  std::cerr << "Storage type " << (int)frame_->storage_type() << std::endl;
  std::cerr << "Width " << (unsigned long)GetWidth() << std::endl;
  std::cerr << "Height " << (unsigned long)GetHeight() << std::endl;
  std::cerr << "Planes " << frame_->NumPlanes(frame_->format()) << std::endl;
}
}  // namespace electron
}  // namespace media
}  // namespace discord
