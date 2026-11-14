// Copyright 2020 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/display/record_replay_render.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/threading/platform_thread.h"
#include "base/base64.h"
#include "base/no_destructor.h"
#include "base/record_replay.h"
#include "base/record_replay_paint.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "cc/animation/animation_events.h"
#include "cc/trees/compositor_commit_data.h"
#include "cc/trees/proxy_common.h"
#include "cc/trees/proxy_main.h"
#include "components/viz/common/display/renderer_settings.h"
#include "components/viz/common/quads/aggregated_render_pass.h"
#include "components/viz/common/quads/compositor_frame.h"
#include "components/viz/common/quads/compositor_render_pass.h"
#include "components/viz/common/quads/compositor_render_pass_draw_quad.h"
#include "components/viz/common/resources/transferable_resource.h"
#include "components/viz/common/surfaces/local_surface_id.h"
#include "components/viz/service/display/software_output_device.h"
#include "components/viz/service/display/software_renderer.h"
#include "components/viz/service/display_embedder/software_output_surface.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "gpu/command_buffer/client/shared_image_interface.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/display_color_spaces.h"

namespace recordreplay {

namespace {

// The tiles rastered in this process, by the mailbox a frame names them by.
// Raster workers add to it and the compositor thread reads it.
struct RasterBuffers {
  base::Lock lock;
  std::vector<scoped_refptr<gpu::ClientSharedImage>> images;
};

RasterBuffers& GetRasterBuffers() {
  static base::NoDestructor<RasterBuffers> buffers;
  return *buffers;
}

viz::SoftwareOutputSurface* gOutputSurface;
viz::SoftwareRenderer* gRenderer;

// Only one surface is drawn: the first one the process tried to draw. A new
// one is taken up after DoResetPaintSurface(), as when a navigation changes
// which surface the page has.
viz::LocalSurfaceId* gSurfaceId;

void InitializeRenderer() {
  gOutputSurface = new viz::SoftwareOutputSurface(
      std::make_unique<viz::SoftwareOutputDevice>());
  gRenderer = new viz::SoftwareRenderer(new viz::RendererSettings(),
                                        new viz::DebugRendererSettings(),
                                        gOutputSurface, nullptr, nullptr);
  gRenderer->Initialize();
  gRenderer->SetHasBackBuffers(true);
}

// A frame's pass as the renderer takes them. Frames here aren't aggregated
// with other surfaces' before being drawn.
std::unique_ptr<viz::AggregatedRenderPass> ToAggregatedPass(
    const viz::CompositorRenderPass& pass) {
  auto copy = std::make_unique<viz::AggregatedRenderPass>(
      pass.shared_quad_state_list.size(), pass.quad_list.size());
  copy->SetAll(viz::AggregatedRenderPassId::FromUnsafeValue(
                   pass.id.GetUnsafeValue()),
               pass.output_rect, pass.damage_rect,
               pass.transform_to_root_target, gfx::ContentColorUsage::kSRGB,
               pass.has_transparent_background, pass.cache_render_pass,
               pass.has_damage_from_contributing_content,
               pass.generate_mipmap);

  if (pass.shared_quad_state_list.empty()) {
    return copy;
  }
  auto sqs_iter = pass.shared_quad_state_list.begin();
  viz::SharedQuadState* copy_sqs = copy->CreateAndAppendSharedQuadState();
  *copy_sqs = **sqs_iter;
  for (auto* quad : pass.quad_list) {
    while (quad->shared_quad_state != *sqs_iter) {
      ++sqs_iter;
      copy_sqs = copy->CreateAndAppendSharedQuadState();
      *copy_sqs = **sqs_iter;
    }
    if (quad->material == viz::DrawQuad::Material::kCompositorRenderPass) {
      const auto* pass_quad =
          viz::CompositorRenderPassDrawQuad::MaterialCast(quad);
      copy->CopyFromAndAppendRenderPassDrawQuad(
          pass_quad, viz::AggregatedRenderPassId::FromUnsafeValue(
                         pass_quad->render_pass_id.GetUnsafeValue()));
    } else {
      copy->CopyFromAndAppendDrawQuad(quad);
    }
  }
  return copy;
}

// The frame being drawn, and the mappings of its tiles that are in use.
const viz::CompositorFrame* gCurrentFrame;
std::vector<std::unique_ptr<gpu::ClientSharedImage::ScopedMapping>>*
    gCurrentMappings;

// What is being painted, while the driver may ask for it.
const SkPixmap* gCurrentPixmap;

char* EncodeBitmapContents(const char* mime_type, int jpeg_quality) {
  if (!gCurrentPixmap || strcmp(mime_type, "image/jpeg")) {
    return nullptr;
  }
  std::optional<std::vector<uint8_t>> data = gfx::JPEGCodec::Encode(
      *gCurrentPixmap, jpeg_quality, SkJpegEncoder::Downsample::k444);
  if (!data) {
    Print("Error: JPEG encoding failed");
    return nullptr;
  }
  return strdup(base::Base64Encode(*data).c_str());
}

char* PaintWhenDiverged(const char* mime_type, int jpeg_quality);

// The driver asks either as it's told a paint finished, on the compositor
// thread, or at any time on the main thread once a replay has left its
// recording, which is a repaint.
char* PaintCallback(const char* mime_type, int jpeg_quality) {
  if (HasDivergedFromRecording()) {
    return PaintWhenDiverged(mime_type, jpeg_quality);
  }
  return EncodeBitmapContents(mime_type, jpeg_quality);
}

// A paint goes like this:
//
// 1. The main thread commits in ProxyMain::BeginMainFrame, posts
//    ProxyImpl::NotifyReadyToCommitOnImpl to the compositor thread, and blocks
//    until that signals it.
// 2. The compositor thread runs NotifyReadyToCommitOnImpl, and at some point
//    lets the main thread go on.
// 3. The compositor thread finishes painting the frame.
//
// What finishes in 3 is the paint that started in 1, and these keep the two
// together the same way when replaying as when recording. It isn't exact: 2
// and 3 are separate tasks, so a frame could be ready to commit before the
// one before it was painted.

// Made on the main thread before it blocks.
std::atomic<size_t> gCurrentPaintBookmark;

// The paint the compositor thread last heard was ready to commit.
std::atomic<size_t> gLastCommitBookmark;

// The paints of the tree last committed and of the tree being drawn, which is
// the last one activated. Compositor thread. A paint goes to the second: the
// compositor can hear of the next commit, and even make it, while it still
// draws the tree before, and across a navigation that next commit can be of
// an empty tree. Its bookmark would then be a paint point at which there is
// nothing to repaint.
size_t gCommittedBookmark;
size_t gActiveBookmark;

// How a repaint is encoded, the event that ends its wait, and its picture.
const char* gRepaintMimeType;
int gRepaintJPEGQuality;
std::atomic<base::WaitableEvent*> gRepaintEvent;
std::atomic<char*> gRepaintResult;

// What a repaint's draw got as far as, said when it comes back with nothing.
std::atomic<int> gRepaintFramesSubmitted;
std::atomic<int> gRepaintFramesForOtherSurface;
std::atomic<int> gRepaintFramesPainted;

base::Lock& ViewportSizeLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}
gfx::Size& ViewportSize() {
  static gfx::Size size;
  return size;
}

cc::ProxyMain* gCurrentCompositorProxy;

// The main thread's live compositor proxies. A process has one for each
// window it renders, e.g. a page and a popup it opened.
std::vector<cc::ProxyMain*>* gCompositorProxies;

// Embed token of `gSurfaceId`, which identifies the window owning the paint
// surface, for repaints on the main thread: `gSurfaceId` belongs to the
// compositor thread.
std::atomic<const base::UnguessableToken*> gSurfaceEmbedToken;

// The proxy to repaint with. Only frames submitted to the paint surface are
// drawn, so a repaint needs the proxy of the window which owns that surface.
// The current proxy is the one which last began a main frame, and with several
// windows in the process that can be another window's: after a page opens a
// popup, either window can own the paint surface while both keep running main
// frames. A repaint with the other window's proxy draws a frame which
// SubmitCompositorFrame ignores, and returns nothing.
cc::ProxyMain* RepaintCompositorProxy() {
  const base::UnguessableToken* embed_token = gSurfaceEmbedToken;
  if (embed_token && gCompositorProxies) {
    for (cc::ProxyMain* proxy : *gCompositorProxies) {
      if (proxy->RecordReplayLocalSurfaceId().embed_token() == *embed_token) {
        return proxy;
      }
    }
  }
  return gCurrentCompositorProxy;
}

char* PaintWhenDiverged(const char* mime_type, int jpeg_quality) {
  CHECK(IsMainThread());
  gCurrentCompositorProxy = RepaintCompositorProxy();
  if (!gCurrentCompositorProxy) {
    return nullptr;
  }
  gRepaintMimeType = mime_type;
  gRepaintJPEGQuality = jpeg_quality;

  // Bring layout up to date on the main thread and commit a paint.
  auto state = std::make_unique<cc::BeginMainFrameAndCommitState>();
  state->mutator_events = std::make_unique<cc::AnimationEvents>();
  state->commit_data = std::make_unique<cc::CompositorCommitData>();
  // The pause can leave a frame's tree pending behind one that was never
  // drawn, so the compositor has to be told about the repaint before its
  // commit, which waits for the pending tree.
  gCurrentCompositorProxy->RecordReplayRepaintPending();
  gCurrentCompositorProxy->RecordReplayBeginMainFrame(std::move(state));

  // A draw that an earlier repaint which timed out still forces doesn't end
  // this one: there's no event to signal until this repaint's commit, which
  // clears the earlier forced draw. Such a draw can still signal after its
  // repaint stopped waiting, so the event outlives repaints.
  static base::NoDestructor<base::WaitableEvent> event;
  event->Reset();
  gRepaintResult = nullptr;
  gRepaintFramesSubmitted = 0;
  gRepaintFramesForOtherSurface = 0;
  gRepaintFramesPainted = 0;
  gRepaintEvent = event.get();

  // Have the compositor thread draw a frame.
  gCurrentCompositorProxy->RecordReplayRepaint();

  if (!event->TimedWait(base::Milliseconds(3000))) {
    Print("[RuntimeError] Failed waiting to get a repaint.");
    gRepaintResult = nullptr;
  }
  gRepaintEvent = nullptr;
  if (!gRepaintResult) {
    Print("[RuntimeError] Repaint drew nothing: %d frames submitted, %d for "
          "another surface, %d painted. The page %s layers and %s visible.",
          gRepaintFramesSubmitted.load(), gRepaintFramesForOtherSurface.load(),
          gRepaintFramesPainted.load(),
          gCurrentCompositorProxy->RecordReplayHasRootLayer() ? "has" : "has no",
          gCurrentCompositorProxy->RecordReplayIsVisible() ? "is" : "isn't");
    // The compositor thread says what it's waiting on, and gets a moment to.
    gCurrentCompositorProxy->RecordReplayDescribeRepaint();
    base::PlatformThread::Sleep(base::Milliseconds(200));
  }
  return gRepaintResult;
}

}  // namespace

static RepaintSharedImageInterfaceGetter gRepaintSharedImageInterfaceGetter;

void SetRepaintSharedImageInterfaceGetter(
    RepaintSharedImageInterfaceGetter getter) {
  gRepaintSharedImageInterfaceGetter = getter;
}

scoped_refptr<gpu::SharedImageInterface> GetRepaintSharedImageInterface() {
  if (!gRepaintSharedImageInterfaceGetter) {
    return nullptr;
  }
  return gRepaintSharedImageInterfaceGetter();
}

void NotifyRasterBuffer(scoped_refptr<gpu::ClientSharedImage> image) {
  if (!image) {
    return;
  }
  RasterBuffers& buffers = GetRasterBuffers();
  base::AutoLock lock(buffers.lock);
  // What nothing else holds any more is a tile that's gone.
  std::erase_if(buffers.images, [](const auto& held) {
    return held->HasOneRef();
  });
  buffers.images.push_back(std::move(image));
}

void SubmitCompositorFrame(const viz::LocalSurfaceId& local_surface_id,
                           const viz::CompositorFrame& frame) {
  CHECK(IsRecordingOrReplaying());
  gRepaintFramesSubmitted++;

  if (TakePaintSurfaceReset()) {
    delete gSurfaceId;
    gSurfaceId = nullptr;
  }
  if (!gSurfaceId) {
    gSurfaceId = new viz::LocalSurfaceId(local_surface_id);
    // Never freed: the main thread may be reading the one before.
    gSurfaceEmbedToken =
        new base::UnguessableToken(local_surface_id.embed_token());
    if (!gRenderer) {
      InitializeRenderer();
    }
  }

  // A surface can have another parent sequence number than it began with, and
  // newer child sequence numbers: the widget allocates one whenever its frame
  // sink is recreated, e.g. after the GPU process restarts.
  if (gSurfaceId->embed_token() == local_surface_id.embed_token() &&
      local_surface_id.child_sequence_number() >
          gSurfaceId->child_sequence_number()) {
    *gSurfaceId = local_surface_id;
  }
  if (gSurfaceId->child_sequence_number() !=
          local_surface_id.child_sequence_number() ||
      gSurfaceId->embed_token() != local_surface_id.embed_token()) {
    gRepaintFramesForOtherSurface++;
    return;
  }

  viz::AggregatedRenderPassList render_passes;
  for (const auto& pass : frame.render_pass_list) {
    render_passes.push_back(ToAggregatedPass(*pass));
  }

  std::vector<std::unique_ptr<gpu::ClientSharedImage::ScopedMapping>> mappings;
  gCurrentFrame = &frame;
  gCurrentMappings = &mappings;
  gRenderer->DrawFrame(&render_passes, 1, frame.size_in_pixels(),
                       gfx::DisplayColorSpaces(), viz::SurfaceDamageRectList(),
                       viz::TrackedElementRects());
  gCurrentFrame = nullptr;
  gCurrentMappings = nullptr;
}

bool IsDrawingFrame() {
  return !!gCurrentFrame;
}

bool PopulateSkBitmapWithResource(SkBitmap* sk_bitmap,
                                  viz::ResourceId resource_id) {
  if (!gCurrentFrame) {
    return false;
  }
  const viz::TransferableResource* transferable = nullptr;
  for (const viz::TransferableResource& resource :
       gCurrentFrame->resource_list) {
    if (resource.id == resource_id) {
      transferable = &resource;
      break;
    }
  }
  if (!transferable) {
    return false;
  }

  scoped_refptr<gpu::ClientSharedImage> image;
  {
    RasterBuffers& buffers = GetRasterBuffers();
    base::AutoLock lock(buffers.lock);
    for (const auto& held : buffers.images) {
      if (held->mailbox() == transferable->mailbox()) {
        image = held;
        break;
      }
    }
  }
  if (!image) {
    return false;
  }
  std::unique_ptr<gpu::ClientSharedImage::ScopedMapping> mapping = image->Map();
  if (!mapping) {
    return false;
  }
  gfx::Size size = mapping->Size();
  SkImageInfo info = SkImageInfo::MakeN32Premul(size.width(), size.height());
  base::span<uint8_t> memory = mapping->GetMemoryForPlane(0);
  size_t stride = mapping->Stride(0);
  if (memory.size() < info.computeByteSize(stride)) {
    return false;
  }
  bool installed = sk_bitmap->installPixels(info, memory.data(), stride);
  gCurrentMappings->push_back(std::move(mapping));
  return installed;
}

void InitPaintCallback() {
  static bool has_callback = false;
  if (!has_callback) {
    has_callback = true;
    SetPaintCallback(PaintCallback);
  }
}

void OnCommitPaint() {
  gCurrentPaintBookmark = PaintStart();
}

void OnReadyToCommit() {
  gLastCommitBookmark.store(gCurrentPaintBookmark, std::memory_order_relaxed);
}

void OnPaintFinished(const SkPixmap& pixmap, const gfx::Size& viewport_size) {
  gCurrentPixmap = &pixmap;
  {
    base::AutoLock lock(ViewportSizeLock());
    ViewportSize() = viewport_size;
  }
  if (HasDivergedFromRecording()) {
    // Probably a repaint, and nothing a recording has a point for either way.
    gRepaintFramesPainted++;
    if (gRepaintEvent) {
      gRepaintResult = EncodeBitmapContents(gRepaintMimeType, gRepaintJPEGQuality);
    }
  } else if (size_t bookmark = RecordReplayValueIfAllowed(
                 "OnPaintFinished bookmark", gActiveBookmark)) {
    // The main thread sets this for each commit, of any of the process's
    // compositors, while this thread draws: with several widgets (a page and
    // its popups) which commit this draw comes after isn't the same on a
    // replay. So what was read is recorded.
    PaintFinished(bookmark);
  }
  gCurrentPixmap = nullptr;
}

void OnCommitOnImpl() {
  gCommittedBookmark = gLastCommitBookmark.load();
}

void OnActivateOnImpl() {
  gActiveBookmark = gCommittedBookmark;
}

void OnRepaintFinished() {
  if (base::WaitableEvent* event = gRepaintEvent.load()) {
    event->Signal();
  }
}

void SetCompositorProxy(cc::ProxyMain* proxy) {
  if (IsMainThread()) {
    gCurrentCompositorProxy = proxy;
    if (!gCompositorProxies) {
      gCompositorProxies = new std::vector<cc::ProxyMain*>();
    }
    if (std::ranges::find(*gCompositorProxies, proxy) ==
        gCompositorProxies->end()) {
      gCompositorProxies->push_back(proxy);
    }
  }
}

void CompositorProxyDestroyed(cc::ProxyMain* proxy) {
  if (gCurrentCompositorProxy == proxy) {
    gCurrentCompositorProxy = nullptr;
  }
  if (gCompositorProxies && IsMainThread()) {
    std::erase(*gCompositorProxies, proxy);
  }
}

gfx::Size GetCurrentViewportPixelSize() {
  base::AutoLock lock(ViewportSizeLock());
  return ViewportSize();
}

}  // namespace recordreplay
