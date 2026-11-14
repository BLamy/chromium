// Copyright 2020 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_VIZ_SERVICE_DISPLAY_RECORD_REPLAY_RENDER_H_
#define COMPONENTS_VIZ_SERVICE_DISPLAY_RECORD_REPLAY_RENDER_H_

#include "base/memory/scoped_refptr.h"
#include "components/viz/common/resources/resource_id.h"
#include "ui/gfx/geometry/size.h"

class SkBitmap;
class SkPixmap;

namespace cc {
class ProxyMain;
}

namespace gpu {
class ClientSharedImage;
class SharedImageInterface;
}

namespace viz {
class CompositorFrame;
class LocalSurfaceId;
}  // namespace viz

namespace recordreplay {

// A renderer process that is being recorded makes compositor frames the usual
// way and sends them to the GPU process to be drawn to the screen. It also
// gives each frame to a renderer of its own, in the process, which draws it
// into a buffer. When replaying, that buffer is what the page looked like:
// it is encoded as an image for whoever replays, at each paint, and again
// whenever a replay that has left its recording is asked to repaint.

// Has the driver ask here for pictures. Main thread.
void InitPaintCallback();

// The main thread has committed changes to the layer tree and is about to
// block until the compositor thread is ready to commit.
void OnCommitPaint();

// The compositor thread has heard of the main thread's commit.
void OnReadyToCommit();

// A tile's pixels, which a frame's resources name by the image's mailbox.
void NotifyRasterBuffer(scoped_refptr<gpu::ClientSharedImage> image);

// A compositor frame is being submitted to the GPU process.
void SubmitCompositorFrame(const viz::LocalSurfaceId& local_surface_id,
                           const viz::CompositorFrame& frame);

// Whether the frame being drawn has a resource with pixels known here, which
// |sk_bitmap| is then given.
bool PopulateSkBitmapWithResource(SkBitmap* sk_bitmap,
                                  viz::ResourceId resource_id);

// Whether this process draws frames with the renderer here, which has no
// resource provider: every resource is one of the frame being drawn.
bool IsDrawingFrame();

// The software output device has been painted to. Compositor thread.
void OnPaintFinished(const SkPixmap& pixmap, const gfx::Size& viewport_size);

// A repaint's forced draw is over, whether or not it drew. Compositor thread.
void OnRepaintFinished();

// The proxy a repaint is asked of, from the main thread.
void SetCompositorProxy(cc::ProxyMain* proxy);
void CompositorProxyDestroyed(cc::ProxyMain* proxy);

// What a repaint's tiles are made with when the compositor has never had a
// frame sink, as at a pause before the page's first one was made: tiles are
// rastered into shared images, and the interface for them comes with a frame
// sink. The renderer says how to get one. Main thread.
using RepaintSharedImageInterfaceGetter =
    scoped_refptr<gpu::SharedImageInterface> (*)();
void SetRepaintSharedImageInterfaceGetter(
    RepaintSharedImageInterfaceGetter getter);
scoped_refptr<gpu::SharedImageInterface> GetRepaintSharedImageInterface();

// The size in pixels of what was last painted.
gfx::Size GetCurrentViewportPixelSize();

}  // namespace recordreplay

#endif  // COMPONENTS_VIZ_SERVICE_DISPLAY_RECORD_REPLAY_RENDER_H_
