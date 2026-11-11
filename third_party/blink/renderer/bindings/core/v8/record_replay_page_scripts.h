// Copyright (c) 2023 Record Replay Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_BINDINGS_CORE_V8_RECORD_REPLAY_PAGE_SCRIPTS_H_
#define THIRD_PARTY_BLINK_RENDERER_BINDINGS_CORE_V8_RECORD_REPLAY_PAGE_SCRIPTS_H_

#include "v8/include/v8-forward.h"

namespace blink {

// Gives a root frame's new main-world window what a replay's own code in the
// page uses: __RECORD_REPLAY_ARGUMENTS__, the commands answered in the page,
// and the scripts registered to run in every root frame.
void RecordReplayInstallRootFrameScripts(v8::Isolate* isolate,
                                         v8::Local<v8::Context> context);

// Gives a new main-world window the annotation hook and the devtools
// stand-ins, which are the root frame's, when the page is being recorded or
// replayed.
void RecordReplayInstallPageScripts(v8::Isolate* isolate,
                                    v8::Local<v8::Context> context,
                                    bool is_main_frame);

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_BINDINGS_CORE_V8_RECORD_REPLAY_PAGE_SCRIPTS_H_
