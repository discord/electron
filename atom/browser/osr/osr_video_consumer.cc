// Copyright (c) 2019 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/osr/osr_video_consumer.h"

#include <utility>

#include "atom/browser/osr/osr_render_widget_host_view.h"
#include "media/base/video_frame_metadata.h"
#include "media/capture/mojom/video_capture_types.mojom.h"
#include "ui/gfx/skbitmap_operations.h"

namespace atom {

OffScreenVideoConsumer::OffScreenVideoConsumer(
    OffScreenRenderWidgetHostView* view,
    OnPaintCallback callback)
    : callback_(callback),
      view_(view),
      video_capturer_(view->CreateVideoCapturer()),
      weak_ptr_factory_(this) {
  video_capturer_->SetResolutionConstraints(view_->SizeInPixels(),
                                            view_->SizeInPixels(), true);
  video_capturer_->SetAutoThrottlingEnabled(false);
  video_capturer_->SetMinSizeChangePeriod(base::TimeDelta());
  video_capturer_->SetFormat(media::PIXEL_FORMAT_ARGB,
                             gfx::ColorSpace::CreateREC709());
  SetFrameRate(view_->GetFrameRate());
}

OffScreenVideoConsumer::~OffScreenVideoConsumer() = default;

void OffScreenVideoConsumer::SetActive(bool active) {
  if (active) {
    video_capturer_->Start(this);
  } else {
    video_capturer_->Stop();
  }
}

void OffScreenVideoConsumer::SetFrameRate(int frame_rate) {
  video_capturer_->SetMinCapturePeriod(base::TimeDelta::FromSeconds(1) /
                                       frame_rate);
}

void OffScreenVideoConsumer::SizeChanged() {
  video_capturer_->SetResolutionConstraints(view_->SizeInPixels(),
                                            view_->SizeInPixels(), true);
  video_capturer_->RequestRefreshFrame();
}

void OffScreenVideoConsumer::OnFrameCaptured(
    base::ReadOnlySharedMemoryRegion data,
    ::media::mojom::VideoFrameInfoPtr info,
    const gfx::Rect& content_rect,
    viz::mojom::FrameSinkVideoConsumerFrameCallbacksPtr callbacks) {
  if (!CheckContentRect(content_rect)) {
    gfx::Size view_size = view_->SizeInPixels();
    video_capturer_->SetResolutionConstraints(view_size, view_size, true);
    video_capturer_->RequestRefreshFrame();
    return;
  }

  if (!data.IsValid()) {
    callbacks->Done();
    return;
  }

  media::VideoFrameMetadata metadata;
  metadata.MergeInternalValuesFrom(info->metadata);
  gfx::Rect damage_rect;

  auto UPDATE_RECT = media::VideoFrameMetadata::CAPTURE_UPDATE_RECT;
  if (!metadata.GetRect(UPDATE_RECT, &damage_rect)) {
    damage_rect = content_rect;
  }

  callback_.Run(
    content_rect.size(),
    damage_rect,
    base::UnsafeSharedMemoryRegion(),
    std::move(data),
    base::BindOnce(&viz::mojom::FrameSinkVideoConsumerFrameCallbacks::Done, std::move(callbacks))
  );
}

void OffScreenVideoConsumer::OnStopped() {}

bool OffScreenVideoConsumer::CheckContentRect(const gfx::Rect& content_rect) {
  gfx::Size view_size = view_->SizeInPixels();
  gfx::Size content_size = content_rect.size();

  if (std::abs(view_size.width() - content_size.width()) > 2) {
    return false;
  }

  if (std::abs(view_size.height() - content_size.height()) > 2) {
    return false;
  }

  return true;
}

}  // namespace atom
