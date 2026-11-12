// Copyright 2023 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/inspector/record_replay_dom.h"

#include <stdlib.h>
#include <string.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/json/string_escape.h"
#include "base/record_replay.h"
#include "base/values.h"
#include "third_party/blink/renderer/bindings/core/v8/local_window_proxy.h"
#include "third_party/blink/renderer/bindings/core/v8/to_v8_traits.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.h"
#include "third_party/blink/renderer/platform/bindings/dom_wrapper_world.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_css_style_declaration.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_css_style_sheet.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_element.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_event_listener_info.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_node.h"
#include "third_party/blink/renderer/core/css/css_style_sheet.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/dom/node.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/inspector/inspected_frames.h"
#include "third_party/blink/renderer/core/inspector/inspector_css_agent.h"
#include "third_party/blink/renderer/core/inspector/inspector_dom_agent.h"
#include "third_party/blink/renderer/core/inspector/inspector_dom_debugger_agent.h"
#include "third_party/blink/renderer/core/inspector/inspector_network_agent.h"
#include "third_party/blink/renderer/core/inspector/inspector_resource_container.h"
#include "third_party/blink/renderer/core/inspector/inspector_resource_content_loader.h"
#include "third_party/blink/renderer/core/inspector/inspector_session_state.h"
#include "third_party/blink/renderer/core/inspector/main_thread_debugger.h"
#include "third_party/blink/renderer/core/inspector/record_replay_dom_script.h"
#include "third_party/blink/renderer/core/inspector/v8_session_holder.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "third_party/blink/renderer/platform/bindings/v8_binding.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/inspector_protocol/crdtp/json.h"
#include "v8/include/v8-inspector.h"
#include "v8/include/v8.h"

extern "C" void V8RecordReplayRegisterEmbedderCommand(
    const char* name,
    char* (*handler)(const char* params));
extern "C" void V8RecordReplaySetGlueNativesInstaller(
    void (*installer)(v8::Isolate* isolate,
                      v8::Local<v8::Context> context,
                      v8::Local<v8::Object> rr));
extern "C" v8_inspector::V8InspectorSession* V8RecordReplayGlueSession(
    v8::Isolate* isolate);
extern "C" v8_inspector::V8InspectorSession* V8RecordReplayGlueSessionForGroup(
    v8::Isolate* isolate,
    int group);

namespace blink {

namespace {

// The agents answer to nobody: what they would tell a DevTools front end goes
// nowhere.
class NullChannel : public protocol::FrontendChannel {
 public:
  void SendProtocolResponse(int,
                            std::unique_ptr<protocol::Serializable>) override {}
  void SendProtocolNotification(
      std::unique_ptr<protocol::Serializable>) override {}
  void FlushProtocolNotifications() override {}
};

// The inspector's agents for one root frame.
class Agents final : public GarbageCollected<Agents> {
 public:
  Agents(v8::Isolate* isolate, LocalFrame* frame)
      : frame_(frame),
        inspected_frames_(MakeGarbageCollected<InspectedFrames>(frame)),
        session_state_(std::make_unique<InspectorSessionState>(nullptr)),
        dispatcher_(std::make_unique<protocol::UberDispatcher>(&channel_)) {
    // The session is the replay's own, which outlives every page.
    std::shared_ptr<v8_inspector::V8InspectorSession> session(
        V8RecordReplayGlueSessionForGroup(
            isolate, MainThreadDebugger::Instance(isolate)->ContextGroupId(frame)),
        [](v8_inspector::V8InspectorSession*) {});
    dom_ = MakeGarbageCollected<InspectorDOMAgent>(isolate,
                                                   inspected_frames_.Get());
    Init(dom_.Get(), session);
    dom_->RecordReplaySetStandalone();
    network_ = MakeGarbageCollected<InspectorNetworkAgent>(
        inspected_frames_.Get(), nullptr, true);
    Init(network_.Get(), session);
    css_ = MakeGarbageCollected<InspectorCSSAgent>(
        dom_.Get(), inspected_frames_.Get(), network_.Get(),
        MakeGarbageCollected<InspectorResourceContentLoader>(frame),
        MakeGarbageCollected<InspectorResourceContainer>(
            inspected_frames_.Get()));
    Init(css_.Get(), session);
  }

  LocalFrame* frame() const { return frame_.Get(); }

  // The agents know the frame's document as it is now.
  InspectorDOMAgent* dom() {
    dom_->FrameDocumentUpdated(frame_.Get());
    return dom_.Get();
  }
  InspectorCSSAgent* css() {
    dom();
    css_->RecordReplayEnableStandalone();
    return css_.Get();
  }

  void Trace(Visitor* visitor) const {
    visitor->Trace(frame_);
    visitor->Trace(inspected_frames_);
    visitor->Trace(dom_);
    visitor->Trace(network_);
    visitor->Trace(css_);
  }

 private:
  void Init(InspectorAgent* agent,
            std::shared_ptr<v8_inspector::V8InspectorSession> session) {
    agent->Init(frame_->GetProbeSink(), dispatcher_.get(), session_state_.get(),
                V8SessionHolder(std::move(session)), ClientIsTrusted(true));
  }

  Member<LocalFrame> frame_;
  Member<InspectedFrames> inspected_frames_;
  NullChannel channel_;
  std::unique_ptr<InspectorSessionState> session_state_;
  std::unique_ptr<protocol::UberDispatcher> dispatcher_;
  Member<InspectorDOMAgent> dom_;
  Member<InspectorNetworkAgent> network_;
  Member<InspectorCSSAgent> css_;
};

WeakPersistent<LocalFrame>& RootFrame() {
  DEFINE_STATIC_LOCAL(WeakPersistent<LocalFrame>, frame, ());
  return frame;
}

Persistent<Agents>& CurrentAgents() {
  DEFINE_STATIC_LOCAL(Persistent<Agents>, agents, ());
  return agents;
}

// The agents for the root frame, or null when there's no page.
Agents* GetAgents(v8::Isolate* isolate) {
  LocalFrame* frame = RootFrame().Get();
  if (!frame || !frame->GetDocument()) {
    return nullptr;
  }
  Persistent<Agents>& agents = CurrentAgents();
  if (!agents || agents->frame() != frame) {
    agents = MakeGarbageCollected<Agents>(isolate, frame);
  }
  return agents.Get();
}

std::string ToJson(const protocol::Serializable& value) {
  std::vector<uint8_t> cbor = value.Serialize();
  std::string json;
  crdtp::json::ConvertCBORToJSON(crdtp::SpanFrom(cbor), &json);
  return json;
}

template <typename Container>
std::string ArrayToJson(const Container& values) {
  std::string json = "[";
  for (const auto& value : values) {
    if (json.size() > 1) {
      json += ",";
    }
    json += ToJson(*value);
  }
  return json + "]";
}

void ReturnString(const v8::FunctionCallbackInfo<v8::Value>& info,
                  const std::string& text) {
  v8::Local<v8::String> string;
  if (v8::String::NewFromUtf8(info.GetIsolate(), text.data(),
                              v8::NewStringType::kNormal,
                              static_cast<int>(text.size()))
          .ToLocal(&string)) {
    info.GetReturnValue().Set(string);
  }
}

// A page object for one of Blink's, as the page's own scripts would see it.
template <typename T>
v8::Local<v8::Value> ToPageValue(LocalFrame* frame, T* object) {
  ScriptState* script_state = RecordReplayMainWorldIfInitialized(frame);
  if (!script_state || !object) {
    return v8::Local<v8::Value>();
  }
  ScriptState::Scope scope(script_state);
  return ToV8Traits<T>::ToV8(script_state, object);
}

// "element", "node", "style", or "" for what isn't any of them.
void NativeDomClassify(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  const char* kind = "";
  if (V8Element::HasInstance(isolate, info[0])) {
    kind = "element";
  } else if (V8Node::HasInstance(isolate, info[0])) {
    kind = "node";
  } else if (V8CSSStyleDeclaration::HasInstance(isolate, info[0])) {
    kind = "style";
  }
  ReturnString(info, kind);
}

// The script with the commands about the DOM, run in the page, where it has
// the page's window. Called with the functions it uses, it returns its own.
void NativeDomBundle(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  LocalFrame* frame = RootFrame().Get();
  ScriptState* script_state = RecordReplayMainWorldIfInitialized(frame);
  if (!script_state || !script_state->ContextIsValid()) {
    return;
  }
  v8::Local<v8::Context> context = script_state->GetContext();
  v8::Context::Scope context_scope(context);
  v8::TryCatch try_catch(isolate);
  v8::ScriptOrigin origin(V8String(isolate, "record-replay-internal"));
  v8::Local<v8::Script> script;
  v8::Local<v8::Value> factory;
  v8::Local<v8::Value> result;
  v8::Local<v8::Value> env = info[0];
  if (!v8::Script::Compile(context, V8String(isolate, kRecordReplayDomScript),
                           &origin)
           .ToLocal(&script) ||
      !script->Run(context).ToLocal(&factory) || !factory->IsFunction() ||
      !factory.As<v8::Function>()
           ->Call(context, v8::Undefined(isolate), 1, &env)
           .ToLocal(&result)) {
    v8::String::Utf8Value error(isolate, try_catch.Exception());
    recordreplay::Print("[RuntimeError] DOM commands didn't load: %s",
                        *error ? *error : "");
    return;
  }
  info.GetReturnValue().Set(result);
}

void NativeDomLayout(const v8::FunctionCallbackInfo<v8::Value>& info) {
  LocalFrame* frame = RootFrame().Get();
  if (frame && frame->GetDocument()) {
    frame->GetDocument()->UpdateStyleAndLayout(DocumentUpdateReason::kInspector);
  }
}

void NativeDevicePixelRatio(const v8::FunctionCallbackInfo<v8::Value>& info) {
  LocalFrame* frame = RootFrame().Get();
  info.GetReturnValue().Set(frame ? frame->DevicePixelRatio() : 0);
}

// Graphics.getDevicePixelRatio, which whoever replays asks at points where
// nothing of the page's may be touched.
char* CommandGetDevicePixelRatio(const char* params) {
  LocalFrame* frame = RootFrame().Get();
  std::string result =
      "{\"ratio\":" + std::to_string(frame ? frame->DevicePixelRatio() : 0) +
      "}";
  return strdup(result.c_str());
}

// The box model of a node as CDP's DOM.getBoxModel gives it, as JSON.
void NativeDomBoxModel(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  Agents* agents = GetAgents(isolate);
  Node* node = V8Node::ToWrappable(isolate, info[0]);
  if (!agents || !node) {
    return;
  }
  InspectorDOMAgent* dom = agents->dom();
  std::unique_ptr<protocol::DOM::BoxModel> model;
  if (!dom->getBoxModel(dom->RecordReplayBindNode(node), std::nullopt,
                        std::nullopt, &model)
           .IsSuccess() ||
      !model) {
    return;
  }
  ReturnString(info, ToJson(*model));
}

// The rules that apply to an element as CDP's CSS.getMatchedStylesForNode
// gives them, as JSON.
void NativeCssMatchedStyles(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  Agents* agents = GetAgents(isolate);
  Element* element = V8Element::ToWrappable(isolate, info[0]);
  if (!agents || !element) {
    return;
  }
  InspectorCSSAgent* css = agents->css();
  int node_id = agents->dom()->RecordReplayBindNode(element);
  std::unique_ptr<protocol::CSS::CSSStyle> inline_style;
  std::unique_ptr<protocol::CSS::CSSStyle> attributes_style;
  std::unique_ptr<protocol::Array<protocol::CSS::RuleMatch>> matched_rules;
  std::unique_ptr<protocol::Array<protocol::CSS::PseudoElementMatches>>
      pseudo_matches;
  std::unique_ptr<protocol::Array<protocol::CSS::InheritedStyleEntry>>
      inherited;
  std::unique_ptr<protocol::Array<protocol::CSS::InheritedPseudoElementMatches>>
      inherited_pseudo;
  std::unique_ptr<protocol::Array<protocol::CSS::CSSKeyframesRule>> keyframes;
  std::unique_ptr<protocol::Array<protocol::CSS::CSSPositionTryRule>>
      position_try;
  std::optional<int> active_position_fallback;
  std::unique_ptr<protocol::Array<protocol::CSS::CSSPropertyRule>>
      property_rules;
  std::unique_ptr<protocol::Array<protocol::CSS::CSSPropertyRegistration>>
      registrations;
  std::unique_ptr<protocol::Array<protocol::CSS::CSSAtRule>> at_rules;
  std::optional<int> parent_layout_node;
  std::unique_ptr<protocol::Array<protocol::CSS::CSSFunctionRule>> functions;
  protocol::Response response = css->getMatchedStylesForNode(
      node_id, &inline_style, &attributes_style, &matched_rules,
      &pseudo_matches, &inherited, &inherited_pseudo, &keyframes,
      &position_try, &active_position_fallback, &property_rules,
      &registrations, &at_rules, &parent_layout_node, &functions);
  if (!response.IsSuccess()) {
    recordreplay::Print("[RuntimeError] getMatchedStylesForNode: %s",
                        response.Message().c_str());
    return;
  }
  std::string json = "{";
  auto add = [&json](const char* name, const std::string& value) {
    if (json.size() > 1) {
      json += ",";
    }
    json += "\"";
    json += name;
    json += "\":" + value;
  };
  if (attributes_style) {
    add("attributesStyle", ToJson(*attributes_style));
  }
  if (matched_rules) {
    add("matchedRules", ArrayToJson(*matched_rules));
  }
  if (pseudo_matches) {
    add("pseudoIdMatches", ArrayToJson(*pseudo_matches));
  }
  if (keyframes) {
    add("keyframesRules", ArrayToJson(*keyframes));
  }
  ReturnString(info, json + "}");
}

// The page's own object for a style sheet the CSS agent gave an ID.
void NativeCssStyleSheet(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  Agents* agents = GetAgents(isolate);
  if (!agents || !info[0]->IsString()) {
    return;
  }
  CSSStyleSheet* sheet = agents->css()->RecordReplayStyleSheet(
      ToCoreString(isolate, info[0].As<v8::String>()));
  v8::Local<v8::Value> value = ToPageValue(agents->frame(), sheet);
  if (!value.IsEmpty()) {
    info.GetReturnValue().Set(value);
  }
}

// The nodes a search of the document finds, as DevTools' own search does it:
// by text, by selector, and by XPath.
void NativeDomSearch(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  Agents* agents = GetAgents(isolate);
  v8::Local<v8::Array> result = v8::Array::New(isolate);
  info.GetReturnValue().Set(result);
  if (!agents || !info[0]->IsString()) {
    return;
  }
  InspectorDOMAgent* dom = agents->dom();
  String search_id;
  int count = 0;
  if (!dom->performSearch(ToCoreString(isolate, info[0].As<v8::String>()),
                          true, &search_id, &count)
           .IsSuccess()) {
    return;
  }
  if (count) {
    std::unique_ptr<protocol::Array<int>> node_ids;
    if (dom->getSearchResults(search_id, 0, count, &node_ids).IsSuccess()) {
      uint32_t written = 0;
      for (int node_id : *node_ids) {
        v8::Local<v8::Value> value =
            ToPageValue(agents->frame(), dom->NodeForId(node_id));
        if (!value.IsEmpty()) {
          result->Set(context, written++, value).Check();
        }
      }
    }
  }
  dom->discardSearchResults(search_id);
}

// A node's event listeners: [{type, capture, handler}].
void NativeDomEventListeners(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Array> result = v8::Array::New(isolate);
  info.GetReturnValue().Set(result);
  Node* node = V8Node::ToWrappable(isolate, info[0]);
  if (!node) {
    return;
  }
  V8EventListenerInfoList listeners;
  InspectorDOMDebuggerAgent::CollectEventListeners(isolate, node, info[0], node,
                                                   true, &listeners);
  uint32_t written = 0;
  for (const V8EventListenerInfo& listener : listeners) {
    v8::Local<v8::Object> entry = v8::Object::New(isolate);
    entry
        ->Set(context, V8String(isolate, "type"),
              V8String(isolate, listener.event_type))
        .Check();
    entry
        ->Set(context, V8String(isolate, "capture"),
              v8::Boolean::New(isolate, listener.use_capture))
        .Check();
    entry->Set(context, V8String(isolate, "handler"), listener.effective_function)
        .Check();
    result->Set(context, written++, entry).Check();
  }
}

void InstallNatives(v8::Isolate* isolate,
                    v8::Local<v8::Context> context,
                    v8::Local<v8::Object> rr) {
  // Only a page's main thread has a DOM.
  if (!IsMainThread()) {
    return;
  }
  struct {
    const char* name;
    v8::FunctionCallback callback;
  } natives[] = {
      {"domClassify", NativeDomClassify},
      {"domBundle", NativeDomBundle},
      {"domLayout", NativeDomLayout},
      {"devicePixelRatio", NativeDevicePixelRatio},
      {"domBoxModel", NativeDomBoxModel},
      {"cssMatchedStyles", NativeCssMatchedStyles},
      {"cssStyleSheet", NativeCssStyleSheet},
      {"domSearch", NativeDomSearch},
      {"domEventListeners", NativeDomEventListeners},
  };
  for (const auto& native : natives) {
    v8::Local<v8::Function> function;
    if (v8::Function::New(context, native.callback).ToLocal(&function)) {
      rr->Set(context, V8String(isolate, native.name), function).Check();
    }
  }
}

// The HTML each parser was given, by the URL of its document. Kept when
// replaying only, which is when it's asked for.
struct HTMLParse {
  // The parser's address, or zero once the parser has gone.
  uintptr_t parser;
  std::string url;
  std::string contents;
};

std::vector<HTMLParse>& HTMLParses() {
  static auto* parses = new std::vector<HTMLParse>();
  return *parses;
}

bool KeepsHTML() {
  return recordreplay::IsReplaying() && IsMainThread();
}

// Target.getHTMLSource: params are {url}.
char* CommandGetHTMLSource(const char* params) {
  std::optional<base::Value> parsed = base::JSONReader::Read(
      params ? params : "{}", base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  const base::DictValue* dict = parsed ? parsed->GetIfDict() : nullptr;
  const std::string* url = dict ? dict->FindString("url") : nullptr;
  std::string contents;
  if (url) {
    for (const HTMLParse& parse : HTMLParses()) {
      if (parse.url == *url) {
        contents = parse.contents;
        break;
      }
    }
  }
  std::string result = "{\"contents\":";
  base::EscapeJSONString(contents, true, &result);
  result += "}";
  return strdup(result.c_str());
}

}  // namespace

void RecordReplayRegisterDomCommands() {
  V8RecordReplaySetGlueNativesInstaller(InstallNatives);
  V8RecordReplayRegisterEmbedderCommand("Target.getHTMLSource",
                                        CommandGetHTMLSource);
  V8RecordReplayRegisterEmbedderCommand("Graphics.getDevicePixelRatio",
                                        CommandGetDevicePixelRatio);
}

ScriptState* RecordReplayMainWorldIfInitialized(LocalFrame* frame) {
  if (!frame) {
    return nullptr;
  }
  v8::Isolate* isolate = ToIsolate(frame);
  LocalWindowProxy* proxy = frame->WindowProxyMaybeUninitialized(
      DOMWrapperWorld::MainWorld(isolate));
  if (!proxy) {
    return nullptr;
  }
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = proxy->ContextIfInitialized();
  if (context.IsEmpty()) {
    return nullptr;
  }
  ScriptState* script_state = ScriptState::From(isolate, context);
  return script_state->ContextIsValid() ? script_state : nullptr;
}

void RecordReplaySetDomRootFrame(LocalFrame* frame) {
  RootFrame() = frame;
}

LocalFrame* RecordReplayDomRootFrame() {
  return RootFrame().Get();
}

void RecordReplayOnHTMLParseStart(const void* parser, const char* url) {
  if (KeepsHTML()) {
    HTMLParses().push_back({reinterpret_cast<uintptr_t>(parser),
                            url ? url : "", std::string()});
  }
}

void RecordReplayOnHTMLParseData(const void* parser, const char* text) {
  if (!KeepsHTML()) {
    return;
  }
  for (HTMLParse& parse : HTMLParses()) {
    if (parse.parser == reinterpret_cast<uintptr_t>(parser)) {
      parse.contents += text;
      break;
    }
  }
}

void RecordReplayOnHTMLParseFinish(const void* parser) {
  if (!KeepsHTML()) {
    return;
  }
  for (HTMLParse& parse : HTMLParses()) {
    if (parse.parser == reinterpret_cast<uintptr_t>(parser)) {
      parse.parser = 0;
    }
  }
}

}  // namespace blink
