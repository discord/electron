#include "shell/browser/api/electron_api_frame_capture.h"

#include <utility>

#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "components/viz/common/surfaces/subtree_capture_id.h"
#include "components/viz/common/surfaces/video_capture_target.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"  // nogncheck
#include "content/browser/renderer_host/render_widget_host_view_base.h"  // nogncheck
#include "content/public/browser/audio_service.h"
#include "content/public/browser/gpu_data_manager.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "gin/object_template_builder.h"
#include "gpu/config/gpu_info.h"
#include "media/base/audio_parameters.h"
#include "media/mojo/mojom/audio_data_pipe.mojom.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "shell/browser/api/electron_api_web_frame_main.h"
#include "shell/browser/capture/remote_consumer_video_capturer.h"
#include "shell/browser/javascript_environment.h"
#include "shell/common/gin_converters/gfx_converter.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/gin_helper/handle.h"
#include "shell/common/gin_helper/object_template_builder.h"

namespace electron::api {

namespace {

// Matches what media::AudioInputDevice requests; bounds the shared memory the
// consumer can make the audio service allocate.
constexpr uint32_t kMaxAudioSharedMemoryCount = 10;

}  // namespace

// static
const mojo::Remote<mojom::ElectronRenderer>& FrameCapture::GetRendererApi(
    content::RenderFrameHost* rfh) {
  // The renderer binds a single ElectronRenderer receiver per frame, so share
  // WebFrameMain's connection rather than opening a competing one.
  v8::Isolate* isolate = JavascriptEnvironment::GetIsolate();
  v8::HandleScope scope(isolate);
  return WebFrameMain::From(isolate, rfh)->GetRendererApi();
}

gin::DeprecatedWrapperInfo FrameCapture::kWrapperInfo = {
    gin::kEmbedderNativeGin};

FrameCapture::Options::Options() = default;
FrameCapture::Options::Options(const Options&) = default;
FrameCapture::Options::~Options() = default;

// static
gin_helper::Handle<FrameCapture> FrameCapture::Create(
    v8::Isolate* isolate,
    content::RenderFrameHost* target,
    content::RenderFrameHost* consumer,
    const Options& options) {
  auto handle = gin_helper::CreateHandle(
      isolate, new FrameCapture(target, consumer, options));
  handle->Pin(isolate);
  handle->Start();
  return handle;
}

FrameCapture::FrameCapture(content::RenderFrameHost* target,
                           content::RenderFrameHost* consumer,
                           const Options& options)
    : content::WebContentsObserver(
          content::WebContents::FromRenderFrameHost(target)),
      options_(options),
      frame_tree_node_id_(target->GetFrameTreeNodeId()) {
  if (options_.capture_id.empty()) {
    options_.capture_id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  }

#if BUILDFLAG(IS_WIN)
  const gpu::GPUInfo gpu_info =
      content::GpuDataManager::GetInstance()->GetGPUInfo();
  adapter_luid_low_ = gpu_info.active_gpu().luid.LowPart;
  adapter_luid_high_ = gpu_info.active_gpu().luid.HighPart;
#endif

  const auto& consumer_api = GetRendererApi(consumer);
  if (consumer_api.is_bound()) {
    consumer_api->BindFrameCapture(
        options_.capture_id,
        mojom::FrameCaptureParams::New(adapter_luid_low_, adapter_luid_high_),
                                   session_.BindNewPipeAndPassReceiver(),
                                   host_receiver_.BindNewPipeAndPassRemote());
    session_.set_disconnect_handler(base::BindOnce(
        &FrameCapture::OnConsumerDisconnected, weak_factory_.GetWeakPtr()));
    host_receiver_.set_disconnect_handler(base::BindOnce(
        &FrameCapture::OnConsumerDisconnected, weak_factory_.GetWeakPtr()));
  }
}

FrameCapture::~FrameCapture() = default;

void FrameCapture::Start() {
  if (!session_.is_bound()) {
    StopWithReason("consumer-unavailable");
    return;
  }

  capturer_ = std::make_unique<RemoteConsumerVideoCapturer>(
      options_.pixel_format,
      base::BindRepeating(&FrameCapture::AttachVideoConsumer,
                          weak_factory_.GetWeakPtr()));
  capturer_->SetFrameRate(options_.max_frame_rate);
  capturer_->SetMaxSize(options_.max_size);

  capturer_count_ = web_contents()->IncrementCapturerCount(
      gfx::Size(), /*stay_hidden=*/false, /*stay_awake=*/true,
      /*is_activity=*/true);

  Retarget();

#if BUILDFLAG(IS_LINUX)
  // Linux falls back to shared memory when the GPU cannot export native
  // pixmaps, so ask for exportable buffers explicitly.
  capturer_->Start(
      viz::mojom::BufferFormatPreference::kPreferSharedImageWithNativeHandle);
#else
  capturer_->Start(
      viz::mojom::BufferFormatPreference::kPreferMappableSharedImage);
#endif

  if (options_.audio) {
    StartAudio();
  }
}

void FrameCapture::StartAudio() {
  if (session_.is_bound()) {
    session_->StartAudio();
  }
}

void FrameCapture::StopAudio() {
  audio_observers_.Clear();
  if (session_.is_bound()) {
    session_->StopAudio();
  }
}

void FrameCapture::AttachVideoConsumer(
    mojo::PendingReceiver<viz::mojom::FrameSinkVideoConsumer> consumer) {
  if (session_.is_bound()) {
    session_->AttachVideoConsumer(std::move(consumer));
  }
}

content::RenderFrameHost* FrameCapture::GetTargetFrame() const {
  if (!web_contents()) {
    return nullptr;
  }
  return web_contents()->UnsafeFindFrameByFrameTreeNodeId(frame_tree_node_id_);
}

void FrameCapture::Retarget() {
  if (stopped_ || !capturer_) {
    return;
  }

  auto* rfh =
      static_cast<content::RenderFrameHostImpl*>(GetTargetFrame());
  if (!rfh || !rfh->IsRenderFrameLive()) {
    return;
  }

  auto* view = static_cast<content::RenderWidgetHostViewBase*>(rfh->GetView());
  if (!view || !view->GetFrameSinkId().is_valid()) {
    return;
  }

  const uint64_t retarget_id = ++retarget_id_;

  if (rfh->is_local_root()) {
    target_lease_.reset();
    target_kind_ = rfh->GetParent() ? "oopif" : "main-frame";
    ApplyTarget(view->GetFrameSinkId(), std::nullopt);
    return;
  }

  // Same-process iframe: its pixels live in the local root's compositor frame.
  // Tag the owner element for element capture and capture that subtree.
  content::RenderFrameHost* parent = rfh->GetParent();
  if (!parent || !parent->IsRenderFrameLive()) {
    return;
  }
  const auto& parent_api = GetRendererApi(parent);
  if (!parent_api.is_bound()) {
    return;
  }
  // The old lease and this call travel on different pipes, so the renderer
  // could see the old lease close first and briefly drop the element's
  // eligibility. Hold it until the reply, which follows the new lease.
  mojo::Remote<mojom::FrameCaptureTargetLease> previous_lease =
      std::move(target_lease_);
  target_lease_.reset();
  parent_api->EnsureChildFrameCaptureTarget(
      rfh->GetFrameToken(), target_lease_.BindNewPipeAndPassReceiver(),
      base::BindOnce(&FrameCapture::OnSubtreeCaptureTarget,
                     weak_factory_.GetWeakPtr(), retarget_id,
                     view->GetFrameSinkId(), std::move(previous_lease)));
}

void FrameCapture::OnSubtreeCaptureTarget(
    uint64_t retarget_id,
    const viz::FrameSinkId& frame_sink_id,
    mojo::Remote<mojom::FrameCaptureTargetLease> previous_lease,
    const std::optional<base::Token>& token) {
  if (stopped_ || retarget_id != retarget_id_) {
    return;
  }
  if (!token) {
    StopWithReason("target-unsupported");
    return;
  }
  target_kind_ = "subtree";
  ApplyTarget(frame_sink_id, token);
}

void FrameCapture::ApplyTarget(
    const viz::FrameSinkId& frame_sink_id,
    const std::optional<base::Token>& subtree_token) {
  if (frame_sink_id == current_frame_sink_id_ &&
      subtree_token == current_subtree_token_) {
    return;
  }
  current_frame_sink_id_ = frame_sink_id;
  current_subtree_token_ = subtree_token;

  if (subtree_token) {
    capturer_->ChangeTarget(viz::VideoCaptureTarget(
        frame_sink_id, viz::SubtreeCaptureId(*subtree_token)));
  } else {
    capturer_->ChangeTarget(viz::VideoCaptureTarget(frame_sink_id));
  }
  capturer_->RequestRefreshFrame();

  if (!started_) {
    started_ = true;
    // Retargeting can happen inside Create(), before JS has had a chance to
    // attach listeners.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&FrameCapture::EmitStarted,
                                  weak_factory_.GetWeakPtr()));
  }
}

void FrameCapture::EmitStarted() {
  if (stopped_) {
    return;
  }
  v8::Isolate* isolate = JavascriptEnvironment::GetIsolate();
  v8::HandleScope scope(isolate);
  auto details = gin_helper::Dictionary::CreateEmpty(isolate);
  details.Set("captureId", options_.capture_id);
  details.Set("targetKind", target_kind_);
#if BUILDFLAG(IS_WIN)
  auto luid = gin_helper::Dictionary::CreateEmpty(isolate);
  luid.Set("low", adapter_luid_low_);
  luid.Set("high", adapter_luid_high_);
  details.Set("adapterLuid", luid);
#endif
  Emit("started", details);

  if (auto* rfh =
          static_cast<content::RenderFrameHostImpl*>(GetTargetFrame())) {
    SetPaused(rfh->visibility() !=
              blink::mojom::FrameVisibility::kRenderedInViewport);
  }
}

// A frame that isn't rendered, or is outside the viewport (where cross-origin
// frames are render-throttled), produces no new frames.
void FrameCapture::SetPaused(bool paused) {
  if (stopped_ || paused == paused_) {
    return;
  }
  paused_ = paused;
  Emit(paused ? "paused" : "resumed");
}

void FrameCapture::CreateAudioStream(
    mojo::PendingReceiver<media::mojom::AudioInputStream> stream,
    mojo::PendingRemote<media::mojom::AudioInputStreamClient> client,
    const media::AudioParameters& params,
    uint32_t shared_memory_count,
    CreateAudioStreamCallback callback) {
  content::RenderFrameHost* rfh = GetTargetFrame();
  if (stopped_ || !options_.audio || !rfh || !params.IsValid() ||
      !shared_memory_count ||
      shared_memory_count > kMaxAudioSharedMemoryCount) {
    std::move(callback).Run(nullptr);
    return;
  }

  if (!audio_stream_factory_.is_bound()) {
    content::GetAudioService().BindStreamFactory(
        audio_stream_factory_.BindNewPipeAndPassReceiver());
    audio_stream_factory_.reset_on_disconnect();
  }

  mojo::PendingRemote<media::mojom::AudioInputStreamObserver> observer;
  audio_observers_.Add(this, observer.InitWithNewPipeAndPassReceiver());
  audio_stream_factory_->CreateFrameLoopbackStream(
      std::move(stream), std::move(client), std::move(observer), params,
      shared_memory_count, rfh->GetDevToolsFrameToken(),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(callback),
                                                  nullptr));
}

void FrameCapture::OnResized(const gfx::Size& size) {
  if (stopped_ || size == last_frame_size_) {
    return;
  }
  last_frame_size_ = size;
  Emit("resized", size);
}

void FrameCapture::OnConsumerDisconnected() {
  StopWithReason("consumer-gone");
}

void FrameCapture::Stop() {
  StopWithReason("stopped");
}

void FrameCapture::SetConstraints(const gin_helper::Dictionary& constraints) {
  if (!capturer_) {
    return;
  }
  int frame_rate = 0;
  if (constraints.Get("maxFrameRate", &frame_rate) && frame_rate > 0) {
    options_.max_frame_rate = frame_rate;
    capturer_->SetFrameRate(frame_rate);
  }
  gfx::Size max_size;
  if (constraints.Get("maxSize", &max_size)) {
    options_.max_size = max_size;
    capturer_->SetMaxSize(max_size);
  }
  bool audio = false;
  if (constraints.Get("audio", &audio) && audio != options_.audio) {
    options_.audio = audio;
    if (audio) {
      StartAudio();
    } else {
      StopAudio();
    }
  }
}

void FrameCapture::StopWithReason(const std::string& reason) {
  if (stopped_) {
    return;
  }
  stopped_ = true;

  capturer_.reset();
  target_lease_.reset();
  audio_observers_.Clear();
  audio_stream_factory_.reset();
  capturer_count_.RunAndReset();

  // The renderer drops anything still queued from the GPU process and the
  // audio service before telling the native sink the capture stopped.
  if (session_.is_connected()) {
    session_->Stop(reason);
  }
  session_.reset();
  host_receiver_.reset();
  Observe(nullptr);

  // Start() can stop inside Create(), before JS has had a chance to attach
  // listeners. The pin keeps this object alive until the event is delivered.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&FrameCapture::EmitStopped,
                                weak_factory_.GetWeakPtr(), reason));
}

void FrameCapture::EmitStopped(const std::string& reason) {
  Emit("stopped", reason);
  Unpin();
}

void FrameCapture::RenderFrameHostChanged(content::RenderFrameHost* old_host,
                                          content::RenderFrameHost* new_host) {
  if (new_host && new_host->GetFrameTreeNodeId() == frame_tree_node_id_) {
    current_frame_sink_id_ = viz::FrameSinkId();
    current_subtree_token_.reset();
    Retarget();
  }
}

void FrameCapture::RenderFrameCreated(
    content::RenderFrameHost* render_frame_host) {
  if (render_frame_host->GetFrameTreeNodeId() == frame_tree_node_id_) {
    current_frame_sink_id_ = viz::FrameSinkId();
    current_subtree_token_.reset();
    Retarget();
  }
}

void FrameCapture::OnFrameVisibilityChanged(
    content::RenderFrameHost* render_frame_host,
    blink::mojom::FrameVisibility visibility) {
  if (!started_ ||
      render_frame_host->GetFrameTreeNodeId() != frame_tree_node_id_) {
    return;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          &FrameCapture::SetPaused, weak_factory_.GetWeakPtr(),
          visibility != blink::mojom::FrameVisibility::kRenderedInViewport));
}

void FrameCapture::FrameDeleted(content::FrameTreeNodeId frame_tree_node_id) {
  if (frame_tree_node_id == frame_tree_node_id_) {
    StopWithReason("frame-destroyed");
  }
}

void FrameCapture::WebContentsDestroyed() {
  StopWithReason("frame-destroyed");
}

gin::ObjectTemplateBuilder FrameCapture::GetObjectTemplateBuilder(
    v8::Isolate* isolate) {
  return gin_helper::EventEmitterMixin<FrameCapture>::GetObjectTemplateBuilder(
             isolate)
      .SetMethod("stop", &FrameCapture::Stop)
      .SetMethod("setConstraints", &FrameCapture::SetConstraints)
      .SetProperty("captureId", &FrameCapture::CaptureId)
      .SetProperty("capturing", &FrameCapture::IsCapturing);
}

const char* FrameCapture::GetTypeName() {
  return "FrameCapture";
}

}  // namespace electron::api
