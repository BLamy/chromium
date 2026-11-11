// Copyright 2023 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_BINDINGS_CORE_V8_RECORD_REPLAY_NETWORK_H_
#define THIRD_PARTY_BLINK_RENDERER_BINDINGS_CORE_V8_RECORD_REPLAY_NETWORK_H_

#include <stdint.h>

#include "base/containers/span_or_size.h"
#include "third_party/blink/renderer/core/core_export.h"

// A page's network requests, as a recording has them. Each call here is where
// the inspector's own network probe is, and puts a point in the recording.
// What a request was is only worked out when replaying, when whoever replays
// asks for it with a command.

namespace blink {

class Document;
class KURL;
class Resource;
class ResourceError;
class ResourceRequest;
class ResourceResponse;

// A request is about to be sent. |redirect_response| isn't null when this is
// the next hop of a request that was redirected.
CORE_EXPORT void RecordReplayOnNetworkRequest(
    const Document* document,
    const Resource* resource,
    const ResourceRequest& request,
    const ResourceResponse& redirect_response);

// A navigation's request, which the browser sent, was redirected to |new_url|.
CORE_EXPORT void RecordReplayOnNetworkNavigationRedirect(
    uint64_t inspector_id,
    const KURL& new_url,
    const ResourceRequest& request,
    const ResourceResponse& redirect_response);

CORE_EXPORT void RecordReplayOnNetworkResponse(uint64_t inspector_id,
                                               const ResourceResponse& response,
                                               bool from_memory_cache);
CORE_EXPORT void RecordReplayOnNetworkData(uint64_t inspector_id,
                                           base::SpanOrSize<const char> chunk);
CORE_EXPORT void RecordReplayOnNetworkFinished(uint64_t inspector_id,
                                               int64_t encoded_body_length,
                                               int64_t decoded_body_length);
CORE_EXPORT void RecordReplayOnNetworkFailed(uint64_t inspector_id,
                                             const ResourceError& error);

// Has V8 pass the commands about requests on to here. Once per process.
void RecordReplayRegisterNetworkCommands();

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_BINDINGS_CORE_V8_RECORD_REPLAY_NETWORK_H_
