#include "shell/browser/capture/frame_still.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "components/viz/common/frame_sinks/copy_output_request.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "components/viz/host/host_frame_sink_manager.h"
#include "content/browser/compositor/surface_utils.h"  // nogncheck
#include "content/browser/renderer_host/render_frame_host_impl.h"  // nogncheck
#include "content/browser/renderer_host/render_widget_host_view_base.h"  // nogncheck
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/codec/webp_codec.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/size_f.h"
#include "ui/gfx/geometry/vector2d.h"

namespace electron {

namespace {

using StillOutcome = base::expected<FrameStillResult, std::string>;

StillOutcome Encode(SkBitmap bitmap,
                    FrameStillFormat format,
                    int quality,
                    const gfx::Rect& area) {
  std::optional<std::vector<uint8_t>> encoded;
  FrameStillResult result;
  switch (format) {
    case FrameStillFormat::kWebp:
      encoded = gfx::WebpCodec::Encode(bitmap, quality);
      result.mime_type = "image/webp";
      break;
    case FrameStillFormat::kJpeg:
      encoded = gfx::JPEGCodec::Encode(bitmap, quality);
      result.mime_type = "image/jpeg";
      break;
    case FrameStillFormat::kPng:
      encoded = gfx::PNGCodec::FastEncodeBGRASkBitmap(
          bitmap, /*discard_transparency=*/false);
      result.mime_type = "image/png";
      break;
  }
  if (!encoded || encoded->empty()) {
    return base::unexpected("encode-failed");
  }
  result.data = std::move(*encoded);
  result.size = gfx::Size(bitmap.width(), bitmap.height());
  result.area = area;
  return result;
}

void OnCopyResult(FrameStillFormat format,
                  int quality,
                  const gfx::Rect& area,
                  base::ScopedClosureRunner capture_handle,
                  FrameStillCallback callback,
                  std::unique_ptr<viz::CopyOutputResult> result) {
  capture_handle.RunAndReset();
  if (!result || result->IsEmpty()) {
    std::move(callback).Run(base::unexpected("timeout"));
    return;
  }
  SkBitmap bitmap = result->ScopedAccessSkBitmap().GetOutScopedBitmap();
  if (bitmap.drawsNothing()) {
    std::move(callback).Run(base::unexpected("empty"));
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&Encode, std::move(bitmap), format, quality, area),
      std::move(callback));
}

// Scales `rect` from the frame's CSS pixels onto its surface by the ratio of
// the two sizes, which folds in zoom, device scale and any compositing scale.
gfx::Rect MapToSurface(const gfx::Rect& rect,
                       const gfx::SizeF& viewport,
                       const gfx::Size& surface_size) {
  const gfx::Rect bounds(surface_size);
  if (rect.IsEmpty()) {
    return bounds;
  }
  const float sx = surface_size.width() / viewport.width();
  const float sy = surface_size.height() / viewport.height();
  gfx::Rect area = gfx::ToEnclosingRect(gfx::ScaleRect(gfx::RectF(rect), sx, sy));
  area.Intersect(bounds);
  return area;
}

}  // namespace

FrameStillResult::FrameStillResult() = default;
FrameStillResult::FrameStillResult(FrameStillResult&&) = default;
FrameStillResult& FrameStillResult::operator=(FrameStillResult&&) = default;
FrameStillResult::~FrameStillResult() = default;

void CaptureFrameStill(content::RenderFrameHost* rfh,
                       const FrameStillOptions& options,
                       FrameStillCallback callback) {
  auto* rfh_impl = static_cast<content::RenderFrameHostImpl*>(rfh);
  if (!rfh_impl || !rfh_impl->IsRenderFrameLive()) {
    std::move(callback).Run(base::unexpected("frame-gone"));
    return;
  }
  if (!rfh_impl->is_local_root()) {
    std::move(callback).Run(base::unexpected("target-unsupported"));
    return;
  }

  auto* view =
      static_cast<content::RenderWidgetHostViewBase*>(rfh_impl->GetView());
  if (!view || !view->IsSurfaceAvailableForCopy()) {
    std::move(callback).Run(base::unexpected("surface-unavailable"));
    return;
  }
  const gfx::Size surface_size = view->GetCompositorViewportPixelSize();
  if (surface_size.IsEmpty()) {
    std::move(callback).Run(base::unexpected("surface-unavailable"));
    return;
  }

  const gfx::SizeF viewport =
      options.viewport_size.IsEmpty()
          ? gfx::ScaleSize(gfx::SizeF(surface_size),
                           1.f / view->GetDeviceScaleFactor())
          : gfx::SizeF(options.viewport_size);
  const gfx::Rect area = MapToSurface(options.rect, viewport, surface_size);
  if (area.IsEmpty()) {
    std::move(callback).Run(base::unexpected("out-of-bounds"));
    return;
  }

  gfx::Size output =
      options.output_size.IsEmpty() ? area.size() : options.output_size;
  output.SetToMin(area.size());
  output.SetToMax(gfx::Size(1, 1));

  auto* web_contents = content::WebContents::FromRenderFrameHost(rfh_impl);
  base::ScopedClosureRunner capture_handle =
      web_contents->IncrementCapturerCount(gfx::Size(), /*stay_hidden=*/false,
                                           /*stay_awake=*/true,
                                           /*is_activity=*/false);

  auto request = std::make_unique<viz::CopyOutputRequest>(
      viz::CopyOutputRequest::ResultFormat::RGBA,
      viz::CopyOutputRequest::ResultDestination::kSystemMemory,
      base::BindOnce(&OnCopyResult, options.format,
                     std::clamp(options.quality, 0, 100), area,
                     std::move(capture_handle), std::move(callback)));
  request->set_result_task_runner(
      base::SequencedTaskRunner::GetCurrentDefault());
  request->set_area(area);
  if (output != area.size()) {
    request->set_result_selection(gfx::Rect(output));
    request->SetScaleRatio(gfx::Vector2d(area.width(), area.height()),
                           gfx::Vector2d(output.width(), output.height()));
  }

  content::GetHostFrameSinkManager()->RequestCopyOfOutput(
      view->GetCurrentSurfaceId(), std::move(request),
      /*capture_exact_surface_id=*/false, options.timeout);
}

}  // namespace electron
