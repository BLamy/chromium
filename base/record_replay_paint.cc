// Copyright 2024 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/record_replay_paint.h"

#include <dlfcn.h>

#include <atomic>

#include "base/record_replay.h"

namespace recordreplay {

namespace {

// The driver's function of this name, or null in a process that has none.
template <typename T>
T DriverFunction(const char* name) {
  return IsRecordingOrReplaying()
             ? reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name))
             : nullptr;
}

std::atomic<bool> gPaintSurfaceResetRequested{false};

}  // namespace

size_t PaintStart() {
  auto* paint_start = DriverFunction<size_t (*)()>("RecordReplayPaintStart");
  return paint_start ? paint_start() : 0;
}

void PaintFinished(size_t bookmark) {
  auto* paint_finished =
      DriverFunction<void (*)(size_t)>("RecordReplayPaintFinished");
  if (paint_finished && bookmark) {
    paint_finished(bookmark);
  }
}

void SetPaintCallback(char* (*callback)(const char* mime_type,
                                        int jpeg_quality)) {
  auto* set_paint_callback =
      DriverFunction<void (*)(char* (*)(const char*, int))>(
          "RecordReplaySetPaintCallback");
  if (set_paint_callback) {
    set_paint_callback(callback);
  }
}

void DoResetPaintSurface() {
  gPaintSurfaceResetRequested.store(true);
}

bool TakePaintSurfaceReset() {
  bool requested = gPaintSurfaceResetRequested.exchange(false);
  return RecordReplayValue("TakePaintSurfaceReset", requested);
}

}  // namespace recordreplay
