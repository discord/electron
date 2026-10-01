#pragma once

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "electron/shell/common/api/api.mojom.h"
#include "media/audio/audio_input_ipc.h"
#include "media/mojo/mojom/audio_data_pipe.mojom.h"
#include "media/mojo/mojom/audio_input_stream.mojom.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace discord {

// Same as audio::InputIPC, but asks the FrameCapture host in the browser for a
// loopback stream of the captured frame. The browser only brokers creation;
// the data pipe comes back here, so samples flow from the audio service
// straight to this renderer.
class FrameCaptureAudioIPC : public ::media::AudioInputIPC,
                             public ::media::mojom::AudioInputStreamClient {
 public:
  // |host| must outlive this object.
  explicit FrameCaptureAudioIPC(electron::mojom::FrameCaptureHost* host);
  ~FrameCaptureAudioIPC() override;

  // media::AudioInputIPC
  void CreateStream(::media::AudioInputIPCDelegate* delegate,
                    const ::media::AudioParameters& params,
                    bool automatic_gain_control,
                    uint32_t total_segments) override;
  void RecordStream() override;
  void SetVolume(double volume) override;
  void SetOutputDeviceForAec(const std::string& output_device_id) override {}
  void CloseStream() override;

 private:
  // media::mojom::AudioInputStreamClient
  void OnError(::media::mojom::InputStreamErrorCode code) override;
  void OnMutedStateChanged(bool is_muted) override;

  void NotifyError();
  void StreamCreated(::media::mojom::ReadWriteAudioDataPipePtr data_pipe);

  SEQUENCE_CHECKER(sequence_checker_);

  const raw_ptr<electron::mojom::FrameCaptureHost> host_;
  raw_ptr<::media::AudioInputIPCDelegate> delegate_ = nullptr;
  mojo::Remote<::media::mojom::AudioInputStream> stream_;
  mojo::Receiver<::media::mojom::AudioInputStreamClient> client_receiver_{this};

  base::WeakPtrFactory<FrameCaptureAudioIPC> weak_factory_{this};
};

}  // namespace discord
