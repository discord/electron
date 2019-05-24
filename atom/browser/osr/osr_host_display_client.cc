// Copyright (c) 2019 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/osr/osr_host_display_client.h"

#include <utility>

#include "base/memory/shared_memory.h"
#include "components/viz/common/resources/resource_format.h"
#include "components/viz/common/resources/resource_sizes.h"
#include "mojo/public/cpp/system/platform_handle.h"
#include "skia/ext/platform_canvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkRect.h"
#include "third_party/skia/src/core/SkDevice.h"
#include "ui/gfx/skia_util.h"

#if defined(OS_WIN)
#include "skia/ext/skia_utils_win.h"
#endif

namespace atom {

LayeredWindowUpdater::LayeredWindowUpdater(
    viz::mojom::LayeredWindowUpdaterRequest request,
    OnPaintCallback callback)
    : callback_(callback), binding_(this, std::move(request)) {}

LayeredWindowUpdater::~LayeredWindowUpdater() = default;

void LayeredWindowUpdater::SetActive(bool active) {
  active_ = active;
}

void LayeredWindowUpdater::OnAllocatedSharedMemory(
    const gfx::Size& pixel_size,
    mojo::ScopedSharedBufferHandle scoped_buffer_handle) {
  pixel_size_ = pixel_size;
  shm_ =
      mojo::UnwrapUnsafeSharedMemoryRegion(std::move(scoped_buffer_handle));
}

void LayeredWindowUpdater::Draw(const gfx::Rect& damage_rect,
                                DrawCallback draw_callback) {

  callback_.Run(pixel_size_, damage_rect, shm_.Duplicate(), base::ReadOnlySharedMemoryRegion(), std::move(draw_callback));
}

OffScreenHostDisplayClient::OffScreenHostDisplayClient(
    gfx::AcceleratedWidget widget,
    OnPaintCallback callback)
    : viz::HostDisplayClient(widget), callback_(callback) {}
OffScreenHostDisplayClient::~OffScreenHostDisplayClient() {}

void OffScreenHostDisplayClient::SetActive(bool active) {
  active_ = active;
  if (layered_window_updater_) {
    layered_window_updater_->SetActive(active_);
  }
}

void OffScreenHostDisplayClient::IsOffscreen(IsOffscreenCallback callback) {
  std::move(callback).Run(true);
}

void OffScreenHostDisplayClient::CreateLayeredWindowUpdater(
    viz::mojom::LayeredWindowUpdaterRequest request) {
  layered_window_updater_ =
      std::make_unique<LayeredWindowUpdater>(std::move(request), callback_);
  layered_window_updater_->SetActive(active_);
}

}  // namespace atom
