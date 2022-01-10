#ifndef DISCORD_DISCORD_VIDEO_SOURCE_H_
#define DISCORD_DISCORD_VIDEO_SOURCE_H_

#include <memory>
#include <mutex>

#include "discord/electron_video_shared.h"
#include "third_party/blink/public/platform/modules/mediastream/web_media_stream_source.h"
#include "third_party/blink/public/web/modules/mediastream/media_stream_video_source.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/webrtc/api/media_stream_interface.h"

namespace blink {

using DiscordFrameReleaseCB = void (*)(void*);
using discord::media::electron::DiscordFrame;

class MODULES_EXPORT MediaStreamDiscordVideoSource
    : public MediaStreamVideoSource {
 public:
  MediaStreamDiscordVideoSource(const std::string& streamId);
  ~MediaStreamDiscordVideoSource() override;

  MediaStreamDiscordVideoSource(const MediaStreamDiscordVideoSource&) = delete;
  MediaStreamDiscordVideoSource& operator=(
      const MediaStreamDiscordVideoSource&) = delete;

  static void OnFrame(const std::string& streamId,
                      const DiscordFrame& frame,
                      DiscordFrameReleaseCB releaseCB,
                      void* userData);

  base::WeakPtr<MediaStreamVideoSource> GetWeakPtr() override;

 protected:
  // Implements MediaStreamVideoSource.
  void StartSourceImpl(
      VideoCaptureDeliverFrameCB frame_callback,
      EncodedVideoFrameCB encoded_frame_callback,
      VideoCaptureSubCaptureTargetVersionCB sub_capture_target_version_callback,
      VideoCaptureNotifyFrameDroppedCB frame_dropped_callback) override;
  void StopSourceImpl() override;

 private:
  class VideoSourceDelegate;
  scoped_refptr<VideoSourceDelegate> delegate_;
  std::string stream_id_;

  base::WeakPtrFactory<MediaStreamVideoSource> weak_factory_{this};

  static std::unordered_map<std::string, scoped_refptr<VideoSourceDelegate>>*
      by_stream_id_;
  static std::mutex stream_mutex_;
};
}  // namespace blink

#endif  // DISCORD_DISCORD_VIDEO_SOURCE_H_
