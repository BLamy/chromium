// Copyright 2023 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_RECORD_REPLAY_DOM_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_RECORD_REPLAY_DOM_H_

#include "third_party/blink/renderer/core/core_export.h"

// What a replay is told about a page's DOM and styles. The commands are
// answered by a script the replay's own inspector session runs
// (v8/src/replay/replay-glue.js) with the functions here, which get their
// answers from the inspector's DOM and CSS agents, made for the purpose and
// never enabled: no DevTools session is attached.

namespace blink {

class LocalFrame;

// Gives V8 the functions, and the commands answered without a script. Once
// per process.
CORE_EXPORT void RecordReplayRegisterDomCommands();

// The frame the commands are about: the tab's main frame.
CORE_EXPORT void RecordReplaySetDomRootFrame(LocalFrame* frame);
CORE_EXPORT LocalFrame* RecordReplayDomRootFrame();

// A document being parsed, and the text it has been given so far, which is
// what the source of an HTML page is. |parser| only tells them apart.
CORE_EXPORT void RecordReplayOnHTMLParseStart(const void* parser,
                                              const char* url);
CORE_EXPORT void RecordReplayOnHTMLParseData(const void* parser,
                                             const char* text);
CORE_EXPORT void RecordReplayOnHTMLParseFinish(const void* parser);

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_RECORD_REPLAY_DOM_H_
