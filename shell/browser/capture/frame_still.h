#ifndef ELECTRON_SHELL_BROWSER_CAPTURE_FRAME_STILL_H_
#define ELECTRON_SHELL_BROWSER_CAPTURE_FRAME_STILL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/functional/callback_forward.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace content {
class RenderFrameHost;
}

namespace electron {

enum class FrameStillFormat { kWebp, kJpeg, kPng };

struct FrameStillOptions {
  // In the frame's CSS pixels. Empty captures the whole viewport.
  gfx::Rect rect;
  // The frame's viewport in CSS pixels; maps `rect` onto surface pixels.
  gfx::Size viewport_size;
  // Output pixels. Empty keeps the surface resolution; never upscales.
  gfx::Size output_size;
  FrameStillFormat format = FrameStillFormat::kWebp;
  int quality = 92;
  base::TimeDelta timeout = base::Seconds(2);
};

struct FrameStillResult {
  FrameStillResult();
  FrameStillResult(FrameStillResult&&);
  FrameStillResult& operator=(FrameStillResult&&);
  ~FrameStillResult();

  std::vector<uint8_t> data;
  std::string mime_type;
  gfx::Size size;
  // The copied region in surface pixels.
  gfx::Rect area;
};

using FrameStillCallback = base::OnceCallback<void(
    base::expected<FrameStillResult, std::string> result)>;

// Copies the frame's last composited output, cropped and scaled on the GPU,
// and encodes it off the UI thread. Only local roots (main frames and OOPIFs)
// own a surface; other frames fail with "target-unsupported".
void CaptureFrameStill(content::RenderFrameHost* rfh,
                       const FrameStillOptions& options,
                       FrameStillCallback callback);

}  // namespace electron

#endif  // ELECTRON_SHELL_BROWSER_CAPTURE_FRAME_STILL_H_
