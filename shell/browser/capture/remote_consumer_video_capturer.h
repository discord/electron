#ifndef ELECTRON_SHELL_BROWSER_CAPTURE_REMOTE_CONSUMER_VIDEO_CAPTURER_H_
#define ELECTRON_SHELL_BROWSER_CAPTURE_REMOTE_CONSUMER_VIDEO_CAPTURER_H_

#include <optional>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "components/viz/common/surfaces/video_capture_target.h"
#include "media/base/video_types.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/viz/privileged/mojom/compositing/frame_sink_video_capture.mojom.h"
#include "ui/gfx/geometry/size.h"

namespace electron {

// Drives a viz::mojom::FrameSinkVideoCapturer whose consumer lives in another
// process, so captured frames go from the GPU process straight to it without
// a hop through the browser. Like viz::ClientFrameSinkVideoCapturer, it
// re-creates the capturer and replays its settings if the GPU process goes
// away, binding a new consumer each time capture (re)starts.
class RemoteConsumerVideoCapturer {
 public:
  using ConsumerBinder = base::RepeatingCallback<void(
      mojo::PendingReceiver<viz::mojom::FrameSinkVideoConsumer>)>;

  RemoteConsumerVideoCapturer(media::VideoPixelFormat format,
                              ConsumerBinder consumer_binder);
  ~RemoteConsumerVideoCapturer();

  RemoteConsumerVideoCapturer(const RemoteConsumerVideoCapturer&) = delete;
  RemoteConsumerVideoCapturer& operator=(const RemoteConsumerVideoCapturer&) =
      delete;

  void SetFrameRate(int frame_rate);
  // An empty |max_size| removes the limit.
  void SetMaxSize(const gfx::Size& max_size);
  void ChangeTarget(const viz::VideoCaptureTarget& target);
  void RequestRefreshFrame();
  void Start(viz::mojom::BufferFormatPreference preference);

 private:
  void EstablishConnection();
  void OnConnectionError();
  void ApplyResolutionConstraints();
  void StartInternal();

  SEQUENCE_CHECKER(sequence_checker_);

  const media::VideoPixelFormat format_;
  const ConsumerBinder consumer_binder_;

  mojo::Remote<viz::mojom::FrameSinkVideoCapturer> capturer_;
  std::optional<base::TimeDelta> min_capture_period_;
  gfx::Size max_size_;
  std::optional<viz::VideoCaptureTarget> target_;
  uint32_t sub_capture_version_ = 0;
  std::optional<viz::mojom::BufferFormatPreference> preference_;

  base::WeakPtrFactory<RemoteConsumerVideoCapturer> weak_factory_{this};
};

}  // namespace electron

#endif  // ELECTRON_SHELL_BROWSER_CAPTURE_REMOTE_CONSUMER_VIDEO_CAPTURER_H_
