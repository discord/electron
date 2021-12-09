#include "electron/discord/public/discord_video_frame.h"

#include <iostream>
#include "media/base/video_types.h"

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

ElectronVideoStatus DiscordVideoFrame::ToI420(IElectronBuffer* outputBuffer) {
  return ElectronVideoStatus::Failure;
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
