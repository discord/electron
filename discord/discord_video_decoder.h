#pragma once

#include "base/memory/scoped_refptr.h"
#include "discord/electron_video_shared.h"
#include "media/base/video_decoder.h"

namespace base {
class SingleThreadTaskRunner;
}  // namespace base

namespace media {
class DecoderBuffer;
class GpuVideoAcceleratorFactories;
class MediaLog;
class VideoFrame;
}  // namespace media

namespace discord {
namespace media {
namespace electron {

class DiscordVideoDecoderMediaThread;

class DiscordVideoDecoder : public ElectronObject<IElectronVideoDecoder> {
 public:
  DiscordVideoDecoder();
  ~DiscordVideoDecoder() override;
  ElectronVideoStatus Initialize(IElectronVideoFormat* format,
                                 ElectronVideoSink* videoSink,
                                 void* userData) override;
  ElectronVideoStatus SubmitBuffer(IElectronBuffer* buffer,
                                   uint32_t timestamp) override;

 private:
  DiscordVideoDecoderMediaThread* media_thread_state_{};
  bool started_initialize_{false};
  bool initialized_{false};
};

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

class IElectronVideoFrameMedia : public IElectronVideoFrame {
 public:
  static constexpr char IID[] = "IElectronVideoFrameMedia";
  virtual ::media::VideoFrame* GetMediaFrame() = 0;
};

}  // namespace electron
}  // namespace media
}  // namespace discord
