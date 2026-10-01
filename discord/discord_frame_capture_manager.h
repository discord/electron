#pragma once

#include <string>

#include "electron/discord/public/electron_video_shared.h"
#include "electron/shell/common/api/api.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"

namespace discord {

// Routes FrameCapture sessions arriving over mojo to native sinks attached
// through ElectronVideoCreateObject("DiscordFrameCaptureManager"). Sessions
// run on a dedicated capture thread, which receives frames straight from the
// GPU process; audio is read from the audio service on a realtime thread.
class DiscordFrameCaptureManager
    : public media::electron::ElectronObject<
          media::electron::IElectronFrameCaptureManager> {
 public:
  DiscordFrameCaptureManager();

  // May be called on any thread.
  static void BindCapture(
      const std::string& capture_id,
      electron::mojom::FrameCaptureParamsPtr params,
      mojo::PendingReceiver<electron::mojom::FrameCaptureSession> session,
      mojo::PendingRemote<electron::mojom::FrameCaptureHost> host);

  // IElectronFrameCaptureManager. May be called on any thread.
  media::electron::ElectronVideoStatus SetSink(
      char const* captureId,
      media::electron::IElectronFrameCaptureSink* sink) override;

 private:
  ~DiscordFrameCaptureManager() override;
};

}  // namespace discord
