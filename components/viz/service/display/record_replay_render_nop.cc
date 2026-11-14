// Copyright 2020 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// record_replay_render.h for executables that are never recorded and don't
// have the display service, which the compositor's code still names.

#include "components/viz/service/display/record_replay_render.h"

#include "components/viz/common/quads/compositor_frame.h"
#include "components/viz/common/surfaces/local_surface_id.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "gpu/command_buffer/client/shared_image_interface.h"

namespace recordreplay {

void InitPaintCallback() {}
void OnCommitPaint() {}
void OnReadyToCommit() {}
void NotifyRasterBuffer(scoped_refptr<gpu::ClientSharedImage> image) {}
void SubmitCompositorFrame(const viz::LocalSurfaceId& local_surface_id,
                           const viz::CompositorFrame& frame) {}
bool PopulateSkBitmapWithResource(SkBitmap* sk_bitmap,
                                  viz::ResourceId resource_id) {
  return false;
}
bool IsDrawingFrame() {
  return false;
}
void OnPaintFinished(const SkPixmap& pixmap, const gfx::Size& viewport_size) {}
void OnCommitOnImpl() {}
void OnActivateOnImpl() {}
void OnRepaintFinished() {}
void SetRepaintSharedImageInterfaceGetter(
    RepaintSharedImageInterfaceGetter getter) {}
scoped_refptr<gpu::SharedImageInterface> GetRepaintSharedImageInterface() {
  return nullptr;
}
void SetCompositorProxy(cc::ProxyMain* proxy) {}
void CompositorProxyDestroyed(cc::ProxyMain* proxy) {}
gfx::Size GetCurrentViewportPixelSize() {
  return gfx::Size();
}

}  // namespace recordreplay
