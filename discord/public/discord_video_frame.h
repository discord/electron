#pragma once

#include "base/memory/scoped_refptr.h"
#include "electron/discord/public/electron_video_shared.h"
#include "media/base/video_frame.h"

namespace discord {
namespace media {
namespace electron {

class DiscordVideoFormat : public ElectronObject<IElectronVideoFormat> {
 public:
  DiscordVideoFormat();
  ~DiscordVideoFormat() override;
  ElectronVideoStatus SetCodec(ElectronVideoCodec codec) override;
  ElectronVideoCodec GetCodec() override;
  ElectronVideoStatus SetProfile(ElectronVideoCodecProfile profile) override;
  ElectronVideoCodecProfile GetProfile() override;

 private:
  ElectronVideoCodec codec_{ElectronVideoCodec::kCodecH264};
  ElectronVideoCodecProfile profile_{H264PROFILE_MAIN};
};

class IElectronVideoFrameMedia : public IElectronUnknown {
 public:
  static constexpr char IID[] = "IElectronVideoFrameMedia";
  virtual ::media::VideoFrame* GetMediaFrame() = 0;
};

class DiscordVideoFrame : public ElectronObject<IElectronVideoFrameMedia,
                                                IElectronVideoFrame,
                                                IElectronVideoFrameData,
                                                IElectronVideoDebugLoggable> {
 public:
  explicit DiscordVideoFrame(scoped_refptr<::media::VideoFrame> frame);
  ~DiscordVideoFrame() override;

  // IElectronVideoFrame
  uint32_t GetWidth() override;
  uint32_t GetHeight() override;
  uint32_t GetTimestamp() override;
  IElectronVideoFrameData* ToI420() override;

  // IElectronVideoFrameMedia
  ::media::VideoFrame* GetMediaFrame() override;

  // IElectronVideoFrameData
  bool IsMappable() override;
  bool HasTextures() override;
  bool HasMappableGpuBuffer() override;
  ElectronVideoPixelFormat GetFormat() override;
  ElectronVideoStorageType GetStorageType() override;
  int GetStride(size_t plane) override;
  int GetRowBytes(size_t plane) override;
  int GetRows(size_t plane) override;
  uint8_t const* GetData(size_t plane) override;

  // IElectronVideoDebugLoggable
  void PrintDebugLog() override;

 private:
  scoped_refptr<::media::VideoFrame> frame_;
};

}  // namespace electron
}  // namespace media
}  // namespace discord
