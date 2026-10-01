#include "shell/browser/capture/remote_consumer_video_capturer.h"

#include <utility>

#include "base/task/sequenced_task_runner.h"
#include "components/viz/host/host_frame_sink_manager.h"
#include "content/browser/compositor/surface_utils.h"  // nogncheck
#include "media/base/limits.h"
#include "mojo/public/cpp/bindings/pending_remote.h"

namespace electron {

namespace {

// Matches viz::ClientFrameSinkVideoCapturer.
constexpr base::TimeDelta kReEstablishConnectionDelay = base::Milliseconds(100);

}  // namespace

RemoteConsumerVideoCapturer::RemoteConsumerVideoCapturer(
    media::VideoPixelFormat format,
    ConsumerBinder consumer_binder)
    : format_(format), consumer_binder_(std::move(consumer_binder)) {
  EstablishConnection();
}

RemoteConsumerVideoCapturer::~RemoteConsumerVideoCapturer() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (capturer_ && preference_) {
    capturer_->Stop();
  }
}

void RemoteConsumerVideoCapturer::SetFrameRate(int frame_rate) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (frame_rate <= 0) {
    return;
  }
  min_capture_period_ = base::Seconds(1) / frame_rate;
  capturer_->SetMinCapturePeriod(*min_capture_period_);
}

void RemoteConsumerVideoCapturer::SetMaxSize(const gfx::Size& max_size) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  max_size_ = max_size;
  ApplyResolutionConstraints();
}

void RemoteConsumerVideoCapturer::ChangeTarget(
    const viz::VideoCaptureTarget& target) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  target_ = target;
  capturer_->ChangeTarget(target, ++sub_capture_version_);
}

void RemoteConsumerVideoCapturer::RequestRefreshFrame() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  capturer_->RequestRefreshFrame();
}

void RemoteConsumerVideoCapturer::Start(
    viz::mojom::BufferFormatPreference preference) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  preference_ = preference;
  StartInternal();
}

void RemoteConsumerVideoCapturer::EstablishConnection() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  capturer_.reset();
  content::GetHostFrameSinkManager()->CreateVideoCapturer(
      capturer_.BindNewPipeAndPassReceiver());
  capturer_.set_disconnect_handler(
      base::BindOnce(&RemoteConsumerVideoCapturer::OnConnectionError,
                     base::Unretained(this)));

  capturer_->SetFormat(format_);
  capturer_->SetAutoThrottlingEnabled(false);
  capturer_->SetMinSizeChangePeriod(base::TimeDelta());
  capturer_->SetAnimationFpsLockIn(false, 1);
  if (min_capture_period_) {
    capturer_->SetMinCapturePeriod(*min_capture_period_);
  }
  ApplyResolutionConstraints();
  if (target_) {
    capturer_->ChangeTarget(*target_, sub_capture_version_);
  }
  if (preference_) {
    StartInternal();
  }
}

void RemoteConsumerVideoCapturer::OnConnectionError() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&RemoteConsumerVideoCapturer::EstablishConnection,
                     weak_factory_.GetWeakPtr()),
      kReEstablishConnectionDelay);
}

void RemoteConsumerVideoCapturer::ApplyResolutionConstraints() {
  capturer_->SetResolutionConstraints(
      gfx::Size(1, 1),
      max_size_.IsEmpty() ? gfx::Size(media::limits::kMaxDimension,
                                      media::limits::kMaxDimension)
                          : max_size_,
      false);
}

void RemoteConsumerVideoCapturer::StartInternal() {
  mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumer> consumer;
  consumer_binder_.Run(consumer.InitWithNewPipeAndPassReceiver());
  capturer_->Start(std::move(consumer), *preference_);
}

}  // namespace electron
