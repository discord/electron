#include "electron/discord/discord_frame_capture_audio_ipc.h"

#include <utility>

#include "base/functional/bind.h"
#include "media/base/audio_capturer_source.h"

namespace discord {

FrameCaptureAudioIPC::FrameCaptureAudioIPC(
    electron::mojom::FrameCaptureHost* host)
    : host_(host) {}

FrameCaptureAudioIPC::~FrameCaptureAudioIPC() = default;

void FrameCaptureAudioIPC::CreateStream(
    ::media::AudioInputIPCDelegate* delegate,
    const ::media::AudioParameters& params,
    bool automatic_gain_control,
    uint32_t total_segments) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  delegate_ = delegate;

  auto client = client_receiver_.BindNewPipeAndPassRemote();
  client_receiver_.set_disconnect_handler(base::BindOnce(
      &FrameCaptureAudioIPC::NotifyError, base::Unretained(this)));

  host_->CreateAudioStream(
      stream_.BindNewPipeAndPassReceiver(), std::move(client), params,
      total_segments,
      base::BindOnce(&FrameCaptureAudioIPC::StreamCreated,
                     weak_factory_.GetWeakPtr()));
}

void FrameCaptureAudioIPC::RecordStream() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  stream_->Record();
}

void FrameCaptureAudioIPC::SetVolume(double volume) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  stream_->SetVolume(volume);
}

void FrameCaptureAudioIPC::CloseStream() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  delegate_ = nullptr;
  client_receiver_.reset();
  stream_.reset();
  weak_factory_.InvalidateWeakPtrs();
}

void FrameCaptureAudioIPC::OnError(::media::mojom::InputStreamErrorCode code) {
  NotifyError();
}

void FrameCaptureAudioIPC::OnMutedStateChanged(bool is_muted) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (delegate_) {
    delegate_->OnMuted(is_muted);
  }
}

void FrameCaptureAudioIPC::NotifyError() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (delegate_) {
    delegate_->OnError(::media::AudioCapturerSource::ErrorCode::kUnknown);
  }
}

void FrameCaptureAudioIPC::StreamCreated(
    ::media::mojom::ReadWriteAudioDataPipePtr data_pipe) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!delegate_) {
    return;
  }
  if (data_pipe.is_null() || !data_pipe->socket.is_valid_platform_file() ||
      !data_pipe->shared_memory.IsValid()) {
    NotifyError();
    return;
  }
  delegate_->OnStreamCreated(std::move(data_pipe->shared_memory),
                             data_pipe->socket.TakePlatformFile(),
                             /*initially_muted=*/false);
}

}  // namespace discord
