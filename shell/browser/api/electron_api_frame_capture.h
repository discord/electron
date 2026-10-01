#ifndef ELECTRON_SHELL_BROWSER_API_ELECTRON_API_FRAME_CAPTURE_H_
#define ELECTRON_SHELL_BROWSER_API_ELECTRON_API_FRAME_CAPTURE_H_

#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/token.h"
#include "components/viz/common/surfaces/frame_sink_id.h"
#include "content/public/browser/frame_tree_node_id.h"
#include "content/public/browser/web_contents_observer.h"
#include "media/base/video_types.h"
#include "media/mojo/mojom/audio_input_stream.mojom.h"
#include "media/mojo/mojom/audio_stream_factory.mojom.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "shell/browser/event_emitter_mixin.h"
#include "shell/common/api/api.mojom.h"
#include "shell/common/gin_helper/pinnable.h"
#include "shell/common/gin_helper/wrappable.h"
#include "ui/gfx/geometry/size.h"

namespace content {
class RenderFrameHost;
}

namespace gin_helper {
class Dictionary;
template <typename T>
class Handle;
}  // namespace gin_helper

namespace electron {
class RemoteConsumerVideoCapturer;
}  // namespace electron

namespace electron::api {

// Captures the rendered output of a single frame as GPU shared textures (and
// optionally its audio) and streams it to native consumers in another
// renderer. Only control runs here; frames go from the GPU process and audio
// from the audio service straight to the consumer renderer. See
// docs/api/frame-capture.md.
class FrameCapture final : public gin_helper::DeprecatedWrappable<FrameCapture>,
                           public gin_helper::Pinnable<FrameCapture>,
                           public gin_helper::EventEmitterMixin<FrameCapture>,
                           public content::WebContentsObserver,
                           public mojom::FrameCaptureHost,
                           public media::mojom::AudioInputStreamObserver {
 public:
  struct Options {
    Options();
    Options(const Options&);
    ~Options();

    std::string capture_id;
    int max_frame_rate = 60;
    gfx::Size max_size;
    media::VideoPixelFormat pixel_format = media::PIXEL_FORMAT_ARGB;
    bool audio = false;
  };

  static gin_helper::Handle<FrameCapture> Create(
      v8::Isolate* isolate,
      content::RenderFrameHost* target,
      content::RenderFrameHost* consumer,
      const Options& options);

  // gin_helper::Wrappable
  static gin::DeprecatedWrapperInfo kWrapperInfo;
  gin::ObjectTemplateBuilder GetObjectTemplateBuilder(
      v8::Isolate* isolate) override;
  const char* GetTypeName() override;

  // disable copy
  FrameCapture(const FrameCapture&) = delete;
  FrameCapture& operator=(const FrameCapture&) = delete;

 private:
  FrameCapture(content::RenderFrameHost* target,
               content::RenderFrameHost* consumer,
               const Options& options);
  ~FrameCapture() override;

  void Start();
  void StartAudio();
  void StopAudio();

  // JS API
  void Stop();
  void SetConstraints(const gin_helper::Dictionary& constraints);
  const std::string& CaptureId() const { return options_.capture_id; }
  bool IsCapturing() const { return !stopped_; }

  // content::WebContentsObserver
  void RenderFrameHostChanged(content::RenderFrameHost* old_host,
                              content::RenderFrameHost* new_host) override;
  void FrameDeleted(content::FrameTreeNodeId frame_tree_node_id) override;
  void RenderFrameCreated(content::RenderFrameHost* render_frame_host) override;
  void OnFrameVisibilityChanged(
      content::RenderFrameHost* render_frame_host,
      blink::mojom::FrameVisibility visibility) override;
  void WebContentsDestroyed() override;

  // mojom::FrameCaptureHost
  void CreateAudioStream(
      mojo::PendingReceiver<media::mojom::AudioInputStream> stream,
      mojo::PendingRemote<media::mojom::AudioInputStreamClient> client,
      const media::AudioParameters& params,
      uint32_t shared_memory_count,
      CreateAudioStreamCallback callback) override;
  void OnResized(const gfx::Size& size) override;

  // media::mojom::AudioInputStreamObserver
  void DidStartRecording() override {}

  static const mojo::Remote<mojom::ElectronRenderer>& GetRendererApi(
      content::RenderFrameHost* rfh);
  content::RenderFrameHost* GetTargetFrame() const;
  void Retarget();
  void OnSubtreeCaptureTarget(
      uint64_t retarget_id,
      const viz::FrameSinkId& frame_sink_id,
      mojo::Remote<mojom::FrameCaptureTargetLease> previous_lease,
      const std::optional<base::Token>& token);
  void ApplyTarget(const viz::FrameSinkId& frame_sink_id,
                   const std::optional<base::Token>& subtree_token);
  void EmitStarted();
  void EmitStopped(const std::string& reason);
  void SetPaused(bool paused);

  void AttachVideoConsumer(
      mojo::PendingReceiver<viz::mojom::FrameSinkVideoConsumer> consumer);
  void OnConsumerDisconnected();

  void StopWithReason(const std::string& reason);

  Options options_;
  const content::FrameTreeNodeId frame_tree_node_id_;

  mojo::Remote<mojom::FrameCaptureSession> session_;
  mojo::Receiver<mojom::FrameCaptureHost> host_receiver_{this};
  std::unique_ptr<RemoteConsumerVideoCapturer> capturer_;
  base::ScopedClosureRunner capturer_count_;

  mojo::Remote<media::mojom::AudioStreamFactory> audio_stream_factory_;
  // Holding the observer end keeps the loopback stream alive; clearing it
  // makes the audio service close the stream.
  mojo::ReceiverSet<media::mojom::AudioInputStreamObserver> audio_observers_;

  viz::FrameSinkId current_frame_sink_id_;
  std::optional<base::Token> current_subtree_token_;
  // Keeps a same-process target's owner element eligible for capture.
  mojo::Remote<mojom::FrameCaptureTargetLease> target_lease_;
  uint64_t retarget_id_ = 0;
  std::string target_kind_;
  gfx::Size last_frame_size_;
  bool started_ = false;
  bool stopped_ = false;
  bool paused_ = false;

  uint32_t adapter_luid_low_ = 0;
  int32_t adapter_luid_high_ = 0;

  base::WeakPtrFactory<FrameCapture> weak_factory_{this};
};

}  // namespace electron::api

#endif  // ELECTRON_SHELL_BROWSER_API_ELECTRON_API_FRAME_CAPTURE_H_
