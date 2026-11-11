// Copyright 2024 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_RECORD_REPLAY_PAINT_H_
#define BASE_RECORD_REPLAY_PAINT_H_

#include <stddef.h>

#include "base/base_export.h"

// What a recording has of a page's paints: a point for each, and when
// replaying, the picture, which whoever replays asks for through a callback.
// Here rather than beside the renderer that draws the pictures
// (components/viz/service/display/record_replay_render.h) for the code that
// can't depend on it.

namespace recordreplay {

// A paint is starting, on the main thread. Returns what the paint is known by
// when it finishes.
BASE_EXPORT size_t PaintStart();

// The paint that started with |bookmark| has been drawn. Any thread.
BASE_EXPORT void PaintFinished(size_t bookmark);

// |callback| encodes what is drawn now, as base64 the caller frees, or
// returns null.
BASE_EXPORT void SetPaintCallback(char* (*callback)(const char* mime_type,
                                                    int jpeg_quality));

// The surface being painted is about to change, as after a navigation: the
// next frame submitted says which surface is drawn from then on.
BASE_EXPORT void DoResetPaintSurface();
BASE_EXPORT bool TakePaintSurfaceReset();

}  // namespace recordreplay

#endif  // BASE_RECORD_REPLAY_PAINT_H_
