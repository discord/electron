#ifndef DISCORD_DISCORD_VIDEO_SOURCE_H_
#define DISCORD_DISCORD_VIDEO_SOURCE_H_

#include <memory>
#include <mutex>

#include "base/macros.h"
#include "third_party/blink/public/platform/modules/mediastream/web_media_stream_source.h"
#include "third_party/blink/public/web/modules/mediastream/media_stream_video_source.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/webrtc/api/media_stream_interface.h"

namespace blink {

struct DiscordYUVFrame {
  uint8_t* y;
  uint8_t* u;
  uint8_t* v;
  int32_t y_stride;
  int32_t u_stride;
  int32_t v_stride;
};

struct DiscordFrame {
  int64_t timestamp_us;
  union {
    DiscordYUVFrame yuv;
#if defined(OS_WINDOWS)
    HANDLE texture_handle;
#endif
  } frame;
  int32_t width;
  int32_t height;
  int32_t type;
};
using DiscordFrameReleaseCB = void (*)(void*);

class MODULES_EXPORT MediaStreamDiscordVideoSource
    : public MediaStreamVideoSource {
 public:
  MediaStreamDiscordVideoSource(const std::string& streamId);
  ~MediaStreamDiscordVideoSource() override;

  static void OnFrame(const std::string& streamId,
                      const DiscordFrame& frame,
                      DiscordFrameReleaseCB releaseCB,
                      void* userData);

 protected:
  // Implements MediaStreamVideoSource.
  void StartSourceImpl(VideoCaptureDeliverFrameCB frame_callback,
                       EncodedVideoFrameCB encoded_frame_callback) override;
  void StopSourceImpl() override;

 private:
  DISALLOW_COPY_AND_ASSIGN(MediaStreamDiscordVideoSource);

  class VideoSourceDelegate;
  scoped_refptr<VideoSourceDelegate> delegate_;
  std::string stream_id_;

  static std::unordered_map<std::string, scoped_refptr<VideoSourceDelegate>>*
      by_stream_id_;
  static std::mutex stream_mutex_;
};
}  // namespace blink

#endif  // DISCORD_DISCORD_VIDEO_SOURCE_H_
