// Copyright 2023 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/bindings/core/v8/record_replay_network.h"

#include <stdlib.h>
#include <string.h>

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/process/process_handle.h"
#include "base/record_replay.h"
#include "base/values.h"
#include "net/base/net_errors.h"
#include "third_party/blink/public/mojom/fetch/fetch_api_request.mojom-blink.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/scriptable_document_parser.h"
#include "third_party/blink/renderer/core/html/html_frame_owner_element.h"
#include "third_party/blink/renderer/platform/bindings/script_forbidden_scope.h"
#include "third_party/blink/renderer/platform/loader/fetch/fetch_initiator_info.h"
#include "third_party/blink/renderer/platform/loader/fetch/fetch_initiator_type_names.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_error.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_request.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_response.h"
#include "third_party/blink/renderer/platform/network/encoded_form_data.h"
#include "third_party/blink/renderer/platform/network/http_names.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"

extern "C" void V8RecordReplayRegisterEmbedderCommand(
    const char* name,
    char* (*handler)(const char* params));

namespace blink {

namespace {

bool ShouldReport() {
  if (!recordreplay::IsRecordingOrReplaying("notify-network") ||
      !recordreplay::IsMainThread() || recordreplay::AreEventsDisallowed()) {
    return false;
  }
  // A page's first requests come before it has a window, which is when the
  // rest of what V8 is given is set up.
  RecordReplayRegisterNetworkCommands();
  return true;
}

// What is known of a request between its being sent and its ending, by the
// inspector's identifier for it. A request that is redirected keeps its
// identifier, and each hop is a request of its own in the recording.
struct RequestState {
  size_t redirect_count = 0;
  bool has_form_body = false;
  std::string form_body;
  uint64_t bookmark = 0;
  std::optional<base::DictValue> request;
  size_t response_data_received = 0;
};

std::map<uint64_t, RequestState>& States() {
  static auto* states = new std::map<uint64_t, RequestState>();
  return *states;
}

// The inspector's identifiers can differ when replaying, because the
// inspector does different things then. The recorded one is used.
uint64_t Identifier(uint64_t inspector_id) {
  return recordreplay::RecordReplayValue("NetworkInspectorId", inspector_id);
}

std::string RequestId(uint64_t identifier) {
  std::string id = std::to_string(static_cast<int>(base::GetCurrentProcId())) +
                   "." + std::to_string(identifier);
  auto it = States().find(identifier);
  if (it != States().end() && it->second.redirect_count) {
    id += ":" + std::to_string(it->second.redirect_count);
  }
  return id;
}

void GraphNode(const char* kind,
               const std::string& request_id,
               const std::string& more = std::string()) {
  if (!recordreplay::DependencyGraphEnabled()) {
    return;
  }
  std::string json =
      recordreplay::DependencyGraphJson(kind, {{"requestId", request_id}});
  json.pop_back();
  json += more + "}";
  recordreplay::NewDependencyGraphNode(json.c_str());
}

const char* HttpVersionName(ResourceResponse::HTTPVersion version) {
  switch (version) {
    case ResourceResponse::kHTTPVersion_0_9:
      return "http/0.9";
    case ResourceResponse::kHTTPVersion_1_0:
      return "http/1.0";
    case ResourceResponse::kHTTPVersion_1_1:
      return "http/1.1";
    case ResourceResponse::kHTTPVersion_2_0:
      return "http/2.0";
    case ResourceResponse::kHTTPVersionUnknown:
      return "http";
  }
  return "http/unknown";
}

const char* RequestCause(const ResourceRequest& request) {
  using Type = mojom::blink::RequestContextType;
  switch (request.GetRequestContext()) {
    case Type::SCRIPT:
      return "script";
    case Type::STYLE:
      return "stylesheet";
    case Type::OBJECT:
      return "object";
    case Type::IMAGE:
      return "img";
    case Type::XML_HTTP_REQUEST:
      return "xhr";
    case Type::BEACON:
      return "beacon";
    case Type::FETCH:
      return "fetch";
    case Type::XSLT:
      return "xslt";
    case Type::MANIFEST:
      return "webManifest";
    case Type::FONT:
      return "font";
    case Type::PING:
      return "ping";
    case Type::IMAGE_SET:
      return "imageset";
    case Type::CSP_REPORT:
      return "csp";
    case Type::IFRAME:
    case Type::FRAME:
    case Type::SUBRESOURCE:
      return "subdocument";
    case Type::HYPERLINK:
    case Type::PREFETCH:
      return "document";
    case Type::VIDEO:
      return "media";
    case Type::FAVICON:
      return "favicon";
    case Type::AUDIO:
      return "audio";
    case Type::DOWNLOAD:
      return "download";
    case Type::EMBED:
      return "embed";
    case Type::EVENT_SOURCE:
      return "eventSource";
    case Type::FORM:
      return "form";
    case Type::INTERNAL:
      return "internal";
    case Type::LOCATION:
      return "location";
    case Type::PLUGIN:
      return "plugin";
    case Type::SERVICE_WORKER:
      return "serviceWorker";
    case Type::SHARED_WORKER:
      return "sharedWorker";
    case Type::SUBRESOURCE_WEBBUNDLE:
      return "subresourceWebbundle";
    case Type::TRACK:
      return "track";
    case Type::WORKER:
      return "worker";
    default:
      return nullptr;
  }
}

template <typename Headers>
base::ListValue HeaderList(const Headers& fields) {
  base::ListValue headers;
  for (const auto& header : fields) {
    base::DictValue entry;
    entry.Set("name", header.key.Utf8());
    entry.Set("value", header.value.Utf8());
    headers.Append(std::move(entry));
  }
  return headers;
}

// As InspectorNetworkAgent::BuildInitiatorObject has it, for what can be
// known without the inspector.
std::optional<base::DictValue> Initiator(const Document* document,
                                         const FetchInitiatorInfo& info) {
  if (info.is_imported_module && !info.referrer.empty()) {
    base::DictValue rv;
    rv.Set("url", info.referrer.Utf8());
    rv.Set("line", info.position.line_.OneBasedInt());
    rv.Set("column", info.position.column_.ZeroBasedInt());
    return rv;
  }
  bool by_stylesheet = info.name == fetch_initiator_type_names::kCSS ||
                       info.name == fetch_initiator_type_names::kUacss;
  if (by_stylesheet && !info.referrer.empty()) {
    base::DictValue rv;
    rv.Set("url", info.referrer.Utf8());
    return rv;
  }
  while (document && !document->GetScriptableDocumentParser()) {
    document = document->LocalOwner() ? document->LocalOwner()->ownerDocument()
                                      : nullptr;
  }
  if (!document) {
    return std::nullopt;
  }
  base::DictValue rv;
  KURL url = document->Url();
  url.RemoveFragmentIdentifier();
  rv.Set("url", url.GetString().Utf8());
  TextPosition position = info.position;
  if (position == TextPosition::BelowRangePosition()) {
    position = document->GetScriptableDocumentParser()->GetTextPosition();
  }
  rv.Set("line", position.line_.OneBasedInt());
  rv.Set("column", position.column_.ZeroBasedInt());
  return rv;
}

// What the commands answer with while the driver is being told something.
const std::string* gCurrentEvent = nullptr;
const uint8_t* gCurrentStreamData = nullptr;
size_t gCurrentStreamSize = 0;

// Tells the driver something happened to a request. When replaying, the
// driver asks what with a command before this returns.
void EmitEvent(const std::string& request_id, base::DictValue event) {
  std::string json;
  if (recordreplay::IsReplaying()) {
    json = base::WriteJson(event).value_or("{}");
  }
  gCurrentEvent = &json;
  recordreplay::OnNetworkRequestEvent(request_id.c_str());
  gCurrentEvent = nullptr;
}

void EmitStreamData(const std::string& stream_id,
                    size_t offset,
                    size_t length,
                    const uint8_t* data,
                    size_t size) {
  gCurrentStreamData = data;
  gCurrentStreamSize = size;
  recordreplay::OnNetworkStreamData(stream_id.c_str(), offset, length, 0);
  gCurrentStreamData = nullptr;
  gCurrentStreamSize = 0;
}

void EmitRequest(const std::string& request_id,
                 uint64_t bookmark,
                 base::DictValue event) {
  recordreplay::OnNetworkRequest(request_id.c_str(), "http", bookmark);
  event.Set("kind", "request");
  EmitEvent(request_id, std::move(event));
}

void EmitResponse(const std::string& request_id,
                  const ResourceResponse& response,
                  bool from_cache) {
  base::DictValue event;
  event.Set("kind", "response");
  if (recordreplay::IsReplaying()) {
    event.Set("responseHeaders", HeaderList(response.HttpHeaderFields()));
    event.Set("responseProtocolVersion",
              HttpVersionName(response.HttpVersion()));
    event.Set("responseStatus", response.HttpStatusCode());
    event.Set("responseStatusText", response.HttpStatusText().Utf8());
    event.Set("responseFromCache", from_cache);
  }
  EmitEvent(request_id, std::move(event));
}

void EmitRequestBody(const std::string& request_id, const std::string& body) {
  base::DictValue event;
  event.Set("kind", "request-body");
  EmitEvent(request_id, std::move(event));
  std::string stream_id = "request-" + request_id;
  recordreplay::OnNetworkStreamStart(stream_id.c_str(), "request-data",
                                     request_id.c_str());
  EmitStreamData(stream_id, 0, body.size(),
                 reinterpret_cast<const uint8_t*>(body.data()), body.size());
}

char* CommandCurrentRequestEvent(const char* params) {
  std::string result = "{\"data\":";
  result += gCurrentEvent && !gCurrentEvent->empty() ? *gCurrentEvent : "{}";
  result += "}";
  return strdup(result.c_str());
}

// Params are {index, length}, a range of the data the driver is being told of.
char* CommandCurrentStreamData(const char* params) {
  std::optional<base::Value> parsed = base::JSONReader::Read(
      params ? params : "{}", base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  const base::DictValue* dict = parsed ? parsed->GetIfDict() : nullptr;
  size_t index = dict ? static_cast<size_t>(
                            dict->FindDouble("index").value_or(0))
                      : 0;
  size_t length = dict ? static_cast<size_t>(
                             dict->FindDouble("length").value_or(0))
                       : 0;
  if (!gCurrentStreamData || index > gCurrentStreamSize ||
      gCurrentStreamSize - index < length) {
    return strdup("{}");
  }
  std::string encoded = base::Base64Encode(
      // SAFETY: the range was checked against the data's size just above.
      UNSAFE_BUFFERS(base::span<const uint8_t>(gCurrentStreamData + index,
                                               length)));
  std::string result = "{\"data\":{\"kind\":\"data\",\"value\":\"" + encoded +
                       "\"}}";
  return strdup(result.c_str());
}

}  // namespace

void RecordReplayRegisterNetworkCommands() {
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  V8RecordReplayRegisterEmbedderCommand("Target.getCurrentNetworkRequestEvent",
                                        CommandCurrentRequestEvent);
  V8RecordReplayRegisterEmbedderCommand("Target.getCurrentNetworkStreamData",
                                        CommandCurrentStreamData);
}

void RecordReplayOnNetworkRequest(const Document* document,
                                  const Resource* resource,
                                  const ResourceRequest& request,
                                  const ResourceResponse& redirect_response) {
  if (!ShouldReport()) {
    return;
  }
  // Telling the driver can have it ask about the page there and then, with
  // the replay's own script, from where the page's scripts aren't allowed.
  ScriptForbiddenScope::AllowUserAgentScript allow_script;
  const uint64_t identifier = Identifier(request.InspectorId());
  const bool is_redirect = !redirect_response.IsNull();

  // Where the request was made, which is where the inspector takes its stack.
  uint64_t bookmark = recordreplay::NewBookmark();

  std::string redirect_source_id;
  if (is_redirect) {
    // The response that redirected ends the hop before this one.
    redirect_source_id = RequestId(identifier);
    EmitResponse(redirect_source_id, redirect_response,
                 redirect_response.WasCached());
    States()[identifier].redirect_count++;
  }
  RequestState& state = States()[identifier];
  state.bookmark = bookmark;
  state.response_data_received = 0;
  std::string request_id = RequestId(identifier);

  GraphNode("networkRequest", request_id);

  base::DictValue event;
  if (recordreplay::IsReplaying()) {
    event.Set("requestUrl", request.Url().GetString().Utf8());
    event.Set("requestMethod", request.HttpMethod().Utf8());
    if (const char* cause = RequestCause(request)) {
      event.Set("requestCause", cause);
    }
    event.Set("requestHeaders", HeaderList(request.HttpHeaderFields()));
    if (resource) {
      if (auto initiator =
              Initiator(document, resource->Options().initiator_info)) {
        event.Set("initiator", std::move(*initiator));
      }
    }
    if (is_redirect) {
      event.Set("redirectSourceId", redirect_source_id);
    }
    state.request = event.Clone();
  }
  EmitRequest(request_id, bookmark, std::move(event));

  // The body, which a redirected request doesn't always still have.
  const scoped_refptr<EncodedFormData>& body = request.HttpBody();
  if (body) {
    state.has_form_body = true;
    state.form_body = body->FlattenToString().Utf8();
    EmitRequestBody(request_id, state.form_body);
  } else if (is_redirect && state.has_form_body &&
             request.HttpMethod() != http_names::kGET &&
             request.HttpMethod() != http_names::kHEAD) {
    EmitRequestBody(request_id, state.form_body);
  }
}

void RecordReplayOnNetworkNavigationRedirect(
    uint64_t inspector_id,
    const KURL& new_url,
    const ResourceRequest& request,
    const ResourceResponse& redirect_response) {
  if (!ShouldReport()) {
    return;
  }
  // Telling the driver can have it ask about the page there and then, with
  // the replay's own script, from where the page's scripts aren't allowed.
  ScriptForbiddenScope::AllowUserAgentScript allow_script;
  const uint64_t identifier = Identifier(inspector_id);
  auto it = States().find(identifier);
  if (it == States().end()) {
    return;
  }
  RequestState& state = it->second;
  std::string previous_id = RequestId(identifier);
  EmitResponse(previous_id, redirect_response, redirect_response.WasCached());

  state.redirect_count++;
  state.response_data_received = 0;
  std::string request_id = RequestId(identifier);

  base::DictValue event;
  if (recordreplay::IsReplaying()) {
    event.Set("requestUrl", new_url.GetString().Utf8());
    event.Set("requestMethod", request.HttpMethod().Utf8());
    event.Set("requestHeaders", HeaderList(request.HttpHeaderFields()));
    if (state.request) {
      for (const char* key : {"requestCause", "initiator"}) {
        if (const base::Value* value = state.request->Find(key)) {
          event.Set(key, value->Clone());
        }
      }
    }
    event.Set("redirectSourceId", previous_id);
    state.request = event.Clone();
  }
  EmitRequest(request_id, state.bookmark, std::move(event));
}

void RecordReplayOnNetworkResponse(uint64_t inspector_id,
                                   const ResourceResponse& response,
                                   bool from_memory_cache) {
  if (!ShouldReport()) {
    return;
  }
  // Telling the driver can have it ask about the page there and then, with
  // the replay's own script, from where the page's scripts aren't allowed.
  ScriptForbiddenScope::AllowUserAgentScript allow_script;
  const uint64_t identifier = Identifier(inspector_id);
  if (!States().count(identifier)) {
    return;
  }
  EmitResponse(RequestId(identifier), response,
               from_memory_cache || response.WasCached());
}

void RecordReplayOnNetworkData(uint64_t inspector_id,
                               base::SpanOrSize<const char> chunk) {
  if (!ShouldReport()) {
    return;
  }
  // Telling the driver can have it ask about the page there and then, with
  // the replay's own script, from where the page's scripts aren't allowed.
  ScriptForbiddenScope::AllowUserAgentScript allow_script;
  const uint64_t identifier = Identifier(inspector_id);
  auto it = States().find(identifier);
  if (it == States().end()) {
    return;
  }
  RequestState& state = it->second;
  std::string request_id = RequestId(identifier);
  const size_t length = chunk.size();

  GraphNode("networkReceiveData", request_id,
            ",\"length\":" + std::to_string(length));

  std::string stream_id = "response-" + request_id;
  if (!state.response_data_received) {
    base::DictValue event;
    event.Set("kind", "response-body");
    EmitEvent(request_id, std::move(event));
    recordreplay::OnNetworkStreamStart(stream_id.c_str(), "response-data",
                                       request_id.c_str());
  }
  if (auto span = chunk.span()) {
    EmitStreamData(stream_id, state.response_data_received, length,
                   reinterpret_cast<const uint8_t*>(span->data()),
                   span->size());
  }
  state.response_data_received += length;
}

void RecordReplayOnNetworkFinished(uint64_t inspector_id,
                                   int64_t encoded_body_length,
                                   int64_t decoded_body_length) {
  if (!ShouldReport()) {
    return;
  }
  // Telling the driver can have it ask about the page there and then, with
  // the replay's own script, from where the page's scripts aren't allowed.
  ScriptForbiddenScope::AllowUserAgentScript allow_script;
  const uint64_t identifier = Identifier(inspector_id);
  if (!States().count(identifier)) {
    return;
  }
  std::string request_id = RequestId(identifier);
  GraphNode("networkFinishLoading", request_id);
  base::DictValue event;
  event.Set("kind", "request-done");
  event.Set("encodedBodySize", static_cast<double>(encoded_body_length));
  event.Set("decodedBodySize", static_cast<double>(decoded_body_length));
  EmitEvent(request_id, std::move(event));
  States().erase(identifier);
}

void RecordReplayOnNetworkFailed(uint64_t inspector_id,
                                 const ResourceError& error) {
  if (!ShouldReport()) {
    return;
  }
  // Telling the driver can have it ask about the page there and then, with
  // the replay's own script, from where the page's scripts aren't allowed.
  ScriptForbiddenScope::AllowUserAgentScript allow_script;
  const uint64_t identifier = Identifier(inspector_id);
  if (!States().count(identifier)) {
    return;
  }
  std::string request_id = RequestId(identifier);
  GraphNode("networkFail", request_id);
  base::DictValue event;
  event.Set("kind", "request-failed");
  event.Set("requestFailedReason",
            net::ErrorToShortString(error.ErrorCode()));
  EmitEvent(request_id, std::move(event));
  States().erase(identifier);
}

}  // namespace blink
