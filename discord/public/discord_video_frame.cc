#include "electron/discord/public/discord_video_frame.h"

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
  return frame_->coded_size().width();
}

uint32_t DiscordVideoFrame::GetHeight() {
  return frame_->coded_size().height();
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
  return frame_->IsMappable();
}

bool DiscordVideoFrame::HasTextures() {
  return frame_->HasTextures();
}

bool DiscordVideoFrame::HasGpuMemoryBuffer() {
  return frame_->HasGpuMemoryBuffer();
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
  return frame_->data(plane);
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
  fprintf(stderr, "HasGpuMemoryBuffer %s\n",
          frame_->HasGpuMemoryBuffer() ? "true" : "false");
  fprintf(stderr, "Format %d\n", (int)frame_->format());
  fprintf(stderr, "Storage type %d\n", (int)frame_->storage_type());
  fprintf(stderr, "Width %lu\n", (unsigned long)GetWidth());
  fprintf(stderr, "Height %lu\n", (unsigned long)GetHeight());
  fprintf(stderr, "Planes %zu\n", frame_->NumPlanes(frame_->format()));
}
}  // namespace electron
}  // namespace media
}  // namespace discord