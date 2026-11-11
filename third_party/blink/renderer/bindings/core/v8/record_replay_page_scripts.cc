// Copyright (c) 2023 Record Replay Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// What a recorded page has of Replay's own: a function for writing an
// annotation into the recording, and stand-ins for the React and Redux
// devtools extensions, which write one each time React commits or a store
// dispatches. The scripts are upstream's, from record_replay_interface.cc on
// Chromium 108. They run in every main-world window, when recording and when
// replaying alike, so they're part of what's recorded.

#include "third_party/blink/renderer/bindings/core/v8/record_replay_page_scripts.h"

#include <stdlib.h>

#include <string>
#include <vector>

#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/bindings/v8_binding.h"
#include "v8/include/v8.h"

// V8's record/replay port keeps to itself, and exports these for its embedder
// (v8/src/replay/replay-glue.cc).
extern "C" void V8RecordReplaySetPageCommandHandler(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    v8::Local<v8::Function> function);
extern "C" uint64_t V8RecordReplayProgressCounter();

namespace blink {

namespace {

const char kArgumentsName[] = "__RECORD_REPLAY_ARGUMENTS__";
const char kApiName[] = "__RECORD_REPLAY__";
const char kInternalScriptName[] = "record-replay-internal";

// What a replay's own code in the page is given: upstream's
// __RECORD_REPLAY_ARGUMENTS__, as far as whoever replays uses it. That's code
// evaluated with Target.evaluatePrivileged, and scripts it registers to run in
// every new root frame before the page's own. This runs with events
// disallowed, on both sides, and isn't instrumented.
const char kPageApiScript[] = R""""(
(() => {
"use strict";
const args = __RECORD_REPLAY_ARGUMENTS__;

// IDs for objects, the same for as long as the object lives.
const persistentIds = new WeakMap();
let nextPersistentId = 1;
function isObject(value) {
  return value !== null && (typeof value === "object" || typeof value === "function");
}
args.getPersistentId = value => {
  if (!isObject(value)) {
    return 0;
  }
  let id = persistentIds.get(value);
  if (!id) {
    id = nextPersistentId++;
    persistentIds.set(value, id);
  }
  return id;
};
args.checkPersistentId = value => (isObject(value) && persistentIds.get(value)) || 0;
args.internal = {};

const commands = {
  "Target.evaluatePrivileged": ({ expression }) => {
    const result = eval(expression);
    return { result };
  },
};

args.setCommandCallback((method, paramsText) => {
  const command = commands[method];
  if (!command) {
    return undefined;
  }
  try {
    return JSON.stringify(command(JSON.parse(paramsText || "{}")) || {});
  } catch (e) {
    args.log(`Error: page command ${method} threw ${e && e.stack || e}`);
    return JSON.stringify({ error: String(e) });
  }
});
})();
)"""";

// Scripts whoever replays wants run in each new root frame, before the page's.
std::vector<std::string>& RootFrameScripts() {
  static std::vector<std::string>* scripts = new std::vector<std::string>();
  return *scripts;
}

void ArgsLog(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::String::Utf8Value text(args.GetIsolate(), args[0]);
  recordreplay::Print("%s", *text ? *text : "");
}

void ArgsGetProgressCounter(const v8::FunctionCallbackInfo<v8::Value>& args) {
  args.GetReturnValue().Set(v8::Number::New(
      args.GetIsolate(), static_cast<double>(V8RecordReplayProgressCounter())));
}

void ArgsRegisterRootFrameScript(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (args.Length() != 1 || !args[0]->IsString()) {
    return;
  }
  v8::String::Utf8Value script(args.GetIsolate(), args[0]);
  if (*script) {
    RootFrameScripts().emplace_back(*script);
  }
}

void ArgsSetCommandCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (args.Length() != 1 || !args[0]->IsFunction()) {
    return;
  }
  v8::Isolate* isolate = args.GetIsolate();
  V8RecordReplaySetPageCommandHandler(isolate, isolate->GetCurrentContext(),
                                      args[0].As<v8::Function>());
}

bool SetHidden(v8::Isolate* isolate,
               v8::Local<v8::Context> context,
               v8::Local<v8::Object> object,
               const char* name,
               v8::Local<v8::Value> value) {
  return object
      ->DefineOwnProperty(context, V8AtomicString(isolate, name), value,
                          v8::DontEnum)
      .FromMaybe(false);
}

bool SetNative(v8::Isolate* isolate,
               v8::Local<v8::Context> context,
               v8::Local<v8::Object> object,
               const char* name,
               v8::FunctionCallback callback) {
  v8::Local<v8::Function> function;
  return v8::Function::New(context, callback).ToLocal(&function) &&
         SetHidden(isolate, context, object, name, function);
}

const char kAnnotationHookName[] = "__RECORD_REPLAY_ANNOTATION_HOOK__";

// Script that injects React DevTools "stub" functions to capture
// marker annotations while recording, for use in later processing
const char kReactDevtoolsScript[] = R""""(
//js
(() => {

const stubFiberRoots = {};
const unmountedFibersByRenderer = {};
const unmountedFiberAlternatesByRenderer = {};

const stubHook = {
  isStub: true,
  supportsFiber: true,
  inject,
  onCommitFiberUnmount,
  onCommitFiberRoot,
  onPostCommitFiberRoot,
  renderers: new Map(),
};


function getFiberRootsSetForRenderer(rendererID) {
  if (!stubFiberRoots[rendererID]) {
    stubFiberRoots[rendererID] = new Set();
  }

  return stubFiberRoots[rendererID];
}

function getUnmountedFibersSetForRenderer(rendererID) {
  if (!unmountedFibersByRenderer[rendererID]) {
    unmountedFibersByRenderer[rendererID] = new Set();
  }

  return unmountedFibersByRenderer[rendererID];
}

function getUnmountedFiberAlternatesForRenderer(rendererID) {
  if (!unmountedFiberAlternatesByRenderer[rendererID]) {
    unmountedFiberAlternatesByRenderer[rendererID] = new Map();
  }

  return unmountedFiberAlternatesByRenderer[rendererID];
}

window.__REACT_DEVTOOLS_SAVED_RENDERERS__ = [];
window.__REACT_DEVTOOLS_STUB_FIBER_ROOTS = stubFiberRoots;

Object.defineProperty(window, "__REACT_DEVTOOLS_GLOBAL_HOOK__", {
  configurable: true,
  enumerable: false,
  get() {
    return stubHook;
  }
});

let uidCounter = 0;

function inject(renderer) {
  // Declare these enum strings in scope for later routine use
  const annotationType = "inject";

  const id = ++uidCounter;
  window.__RECORD_REPLAY_ANNOTATION_HOOK__("react-devtools-hook:v1:" + annotationType, "");
  window.__REACT_DEVTOOLS_SAVED_RENDERERS__.push(renderer);
  return id;
}

function onCommitFiberUnmount(rendererID, fiber) {
  const annotationType = "commit-fiber-unmount"

  // Unmounts are always one fiber at a time during the commit phase.
  // Stash the unmounted fibers here, so we can map them to persistent
  // object IDs inside of `onCommitFiberRoot` processing in the routine.
  const unmountedFibersSet = getUnmountedFibersSetForRenderer(rendererID);
  unmountedFibersSet.add(fiber);

  let unmountedFiberAlternates;
  if (fiber.alternate) {
    unmountedFiberAlternates = getUnmountedFiberAlternatesForRenderer(rendererID);
    unmountedFiberAlternates.set(fiber, fiber.alternate);
  }

  window.__RECORD_REPLAY_ANNOTATION_HOOK__("react-devtools-hook:v1:" + annotationType, "");
}

// eslint-disable-next-line no-unused-vars
function onCommitFiberRoot(rendererID, root, priorityLevel) {
  // The "commit" handler should be the only one the routine needs to do the work as of 2023-05-01.
  // We capture unmounted fibers in the unmount handler above, and the routine
  // will process them when we evaluate at the commit annotation point.
  // The others mostly exist for hypothetical completeness.
  const annotationType = "commit-fiber-root";

  const mountedRoots = getFiberRootsSetForRenderer(rendererID);
  const current = root.current;
  const isKnownRoot = mountedRoots.has(root);
  // Keep track of mounted roots so we can hydrate when DevTools connect.
  const isUnmounting = current.memoizedState == null || current.memoizedState.element == null;

  if (!isKnownRoot && !isUnmounting) {
    mountedRoots.add(root);
  } else if (isKnownRoot && isUnmounting) {
    mountedRoots.delete(root);
  }

  // Get these so it's in scope in the routine eval, and we can clear it after the annotation
  const unmountedFibersSet = getUnmountedFibersSetForRenderer(rendererID);
  const unmountedFiberAlternates = getUnmountedFiberAlternatesForRenderer(rendererID);

  window.__RECORD_REPLAY_ANNOTATION_HOOK__("react-devtools-hook:v1:" + annotationType, "");

  for (const fiber of unmountedFibersSet) {
    unmountedFiberAlternates.delete(fiber);
  }
  unmountedFibersSet.clear();
}

// eslint-disable-next-line no-unused-vars
function onPostCommitFiberRoot(rendererID, root) {
  const annotationType = "post-commit-fiber-root";
  window.__RECORD_REPLAY_ANNOTATION_HOOK__("react-devtools-hook:v1:" + annotationType, "");
}

})();

)"""";



// Script that injects Redux DevTools "stub" functions to capture
// marker annotations while recording, for use in later processing
const char kReduxDevtoolsScript[] = R""""(
//js
(() => { // webpackBootstrap
/******/ 	"use strict";
var __webpack_exports__ = {};

;// CONCATENATED MODULE: ./src/pageScript/api/generateInstanceId.ts
let id = 0;
function generateId(instanceId) {
  return instanceId || ++id;
}
;// CONCATENATED MODULE: ./src/pageScript/api/filters.ts
const FilterState = {
  DO_NOT_FILTER: 'DO_NOT_FILTER',
  DENYLIST_SPECIFIC: 'DENYLIST_SPECIFIC',
  ALLOWLIST_SPECIFIC: 'ALLOWLIST_SPECIFIC'
};
const noFiltersApplied = localFilter => !localFilter && (!window.devToolsOptions || !window.devToolsOptions.filter || window.devToolsOptions.filter === FilterState.DO_NOT_FILTER);
function isFiltered(action, localFilter) {
  if (noFiltersApplied(localFilter) || typeof action !== 'string' && typeof action.type.match !== 'function') {
    return false;
  }
  const {
    allowlist,
    denylist
  } = localFilter || window.devToolsOptions || {};
  const actionType = action.type || action;
  return allowlist && !actionType.match(allowlist) || denylist && actionType.match(denylist);
}
;// CONCATENATED MODULE: ./src/pageScript/api/index.ts


const listeners = {};
function isArray(arg) {
  return Array.isArray(arg);
}
function getLocalFilter(config) {
  const denylist = config.actionsDenylist ?? config.actionsBlacklist;
  const allowlist = config.actionsAllowlist ?? config.actionsWhitelist;
  if (denylist || allowlist) {
    return {
      allowlist: isArray(allowlist) ? allowlist.join('|') : allowlist,
      denylist: isArray(denylist) ? denylist.join('|') : denylist
    };
  }
  return undefined;
}
let latestDispatchedActions = {};
function saveReplayAnnotation(action, state, connectionType, extractedConfig, config) {
  const {
    instanceId
  } = extractedConfig;
  window.__RECORD_REPLAY_ANNOTATION_HOOK__('redux-devtools-setup', JSON.stringify({
    type: 'action',
    actionType: action.type,
    connectionType,
    instanceId
  }));
  latestDispatchedActions[instanceId] = {
    action,
    state,
    extractedConfig,
    config
  };
}
function sendMessage(action, state, preConfig = {}, _instanceId, _name) {
  if (!action || !action.type) {
    action = {
      type: 'update'
    };
  } else if (typeof action === 'string') {
    action = {
      type: action
    };
  }
  const [config, extractedExtensionConfig] = extractExtensionConfig(preConfig);
  saveReplayAnnotation(action, state, 'generic', extractedExtensionConfig, config);
}
function extractExtensionConfig(preConfig) {
  const config = preConfig || {};
  const instanceId = generateId(config.instanceId);
  if (!config.instanceId) config.instanceId = instanceId;
  if (!config.name) {
    config.name = document.title && instanceId === 1 ? document.title : `Instance ${instanceId}`;
  }
  const localFilter = getLocalFilter(config);
  let {
    stateSanitizer,
    actionSanitizer,
    predicate
  } = config;
  const extractedExtensionConfig = {
    instanceId: instanceId,
    stateSanitizer,
    actionSanitizer,
    predicate,
    localFilter,
    isFiltered: isFiltered
  };
  return [config, extractedExtensionConfig];
}
function connect(preConfig) {
  const [config, extractedExtensionConfig] = extractExtensionConfig(preConfig);
  const {
    instanceId
  } = extractedExtensionConfig;
  const subscribe = listener => {
    if (!listener) return undefined;
    return function unsubscribe() {};
  };
  const unsubscribe = () => {
    delete listeners[instanceId];
  };
  const send = (action, state) => {
    if (!action) {
      return;
    }
    let amendedAction = action;
    if (typeof action === 'string') {
      amendedAction = {
        type: action
      };
    }
    saveReplayAnnotation(amendedAction, state, 'generic', extractedExtensionConfig, config);
    return;
  };
  const init = (_state, _liftedData) => {
    window.__RECORD_REPLAY_ANNOTATION_HOOK__('redux-devtools-setup', JSON.stringify({
      type: 'init',
      connectionType: 'generic',
      instanceId
    }));
  };
  const error = (_payload) => {};
  return {
    init,
    subscribe,
    unsubscribe,
    send,
    error
  };
}
;// CONCATENATED MODULE: ./src/pageScript/index.ts


let stores = {};
function __REDUX_DEVTOOLS_EXTENSION__(preConfig = {}) {
  // if (typeof config !== 'object') config = {};
  if (!window.devToolsOptions) window.devToolsOptions = {};
  let store;
  const [config, extractedExtensionConfig] = extractExtensionConfig(preConfig);
  const {
    instanceId
  } = extractedExtensionConfig;
  function init() {
    window.__RECORD_REPLAY_ANNOTATION_HOOK__('redux-devtools-setup', JSON.stringify({
      type: 'init',
      connectionType: 'redux',
      instanceId
    }));
  }
  const enhance = () => next => {
    return (reducer_, initialState_) => {
      const originalStore = next(reducer_, initialState_);
      const newStore = {
        ...originalStore,
        dispatch: action => {
          const result = originalStore.dispatch(action);
          saveReplayAnnotation(action, originalStore.getState(), 'redux', extractedExtensionConfig, config);
          return result;
        }
      };

      // @ts-ignore
      store = stores[instanceId] = newStore;
      init();
      return store;
    };
  };
  return enhance();
}
// noinspection JSAnnotator
window.__REDUX_DEVTOOLS_EXTENSION__ = __REDUX_DEVTOOLS_EXTENSION__;
window.__REDUX_DEVTOOLS_EXTENSION__.open = () => {};
window.__REDUX_DEVTOOLS_EXTENSION__.notifyErrors = () => {};
window.__REDUX_DEVTOOLS_EXTENSION__.send = sendMessage;
window.__REDUX_DEVTOOLS_EXTENSION__.listen = () => {};
window.__REDUX_DEVTOOLS_EXTENSION__.connect = connect;
window.__REDUX_DEVTOOLS_EXTENSION__.disconnect = () => {};
const extensionCompose = config => (...funcs) => {
  // @ts-ignore FIXME
  return (...args) => {
    const instanceId = generateId(config.instanceId);
    return [...funcs].reduceRight(
    // @ts-ignore FIXME
    (composed, f) => f(composed), __REDUX_DEVTOOLS_EXTENSION__({
      ...config,
      instanceId
    })(...args));
  };
};
function reduxDevtoolsExtensionCompose(...funcs) {
  if (funcs.length === 0) {
    return __REDUX_DEVTOOLS_EXTENSION__();
  }
  if (funcs.length === 1 && typeof funcs[0] === 'object') {
    return extensionCompose(funcs[0]);
  }
  return extensionCompose({})(...funcs);
}
window.__REDUX_DEVTOOLS_EXTENSION_COMPOSE__ = reduxDevtoolsExtensionCompose;
/******/ })()

)"""";

// function __RECORD_REPLAY_ANNOTATION_HOOK__(kind, contents)
void InvokeOnAnnotation(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (args.Length() < 2 || !args[0]->IsString()) {
    recordreplay::Print("[RuntimeError] %s called with incorrect arguments",
                        kAnnotationHookName);
    return;
  }
  v8::Isolate* isolate = args.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Object> payload = v8::Object::New(isolate);
  if (payload->Set(context, V8AtomicString(isolate, "message"), args[1])
          .IsNothing()) {
    return;
  }
  v8::Local<v8::String> json;
  if (!v8::JSON::Stringify(context, payload).ToLocal(&json)) {
    recordreplay::Print("[RuntimeError] %s contents failed to json stringify",
                        kAnnotationHookName);
    return;
  }
  v8::String::Utf8Value kind(isolate, args[0]);
  v8::String::Utf8Value contents(isolate, json);
  if (*kind && *contents) {
    recordreplay::OnAnnotation(*kind, *contents);
  }
}

// Named so that the script is a source of the recording: what's read at an
// annotation is read in this script's frames.
bool RunPageScript(v8::Isolate* isolate,
                   v8::Local<v8::Context> context,
                   const char* source_chars,
                   const char* name) {
  v8::TryCatch try_catch(isolate);
  v8::ScriptOrigin origin(V8String(isolate, name));
  v8::Local<v8::Script> script;
  v8::Local<v8::Value> result;
  if (!v8::Script::Compile(context, V8String(isolate, source_chars), &origin)
           .ToLocal(&script) ||
      !script->Run(context).ToLocal(&result)) {
    recordreplay::Print("[RuntimeError] %s didn't run", name);
    return false;
  }
  return true;
}

bool Disabled(const char* name) {
  const char* value = getenv(name);
  return value && value[0] && value[0] != '0';
}

}  // namespace

void RecordReplayInstallRootFrameScripts(v8::Isolate* isolate,
                                         v8::Local<v8::Context> context) {
  if (!recordreplay::IsRecordingOrReplaying()) {
    return;
  }
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context);
  // None of this is the page's, so none of it is in the recording.
  recordreplay::AutoDisallowEvents disallow("RecordReplayRootFrameScripts");
  v8::Local<v8::Object> args = v8::Object::New(isolate);
  v8::Local<v8::Object> api = v8::Object::New(isolate);
  if (!SetNative(isolate, context, args, "log", ArgsLog) ||
      !SetNative(isolate, context, args, "getProgressCounter",
                 ArgsGetProgressCounter) ||
      !SetNative(isolate, context, args, "registerRootFrameScript",
                 ArgsRegisterRootFrameScript) ||
      !SetNative(isolate, context, args, "setCommandCallback",
                 ArgsSetCommandCallback) ||
      !SetHidden(isolate, context, context->Global(), kArgumentsName, args) ||
      !SetHidden(isolate, context, context->Global(), kApiName, api)) {
    return;
  }
  if (!RunPageScript(isolate, context, kPageApiScript, kInternalScriptName)) {
    return;
  }
  // By index: a script may register another.
  for (size_t i = 0; i < RootFrameScripts().size(); i++) {
    std::string script = RootFrameScripts()[i];
    RunPageScript(isolate, context, script.c_str(), kInternalScriptName);
  }
}

void RecordReplayInstallPageScripts(v8::Isolate* isolate,
                                    v8::Local<v8::Context> context) {
  if (!recordreplay::IsRecordingOrReplaying() ||
      recordreplay::AreEventsDisallowed() ||
      recordreplay::HasDivergedFromRecording()) {
    return;
  }
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Function> hook;
  if (!v8::Function::New(context, InvokeOnAnnotation).ToLocal(&hook) ||
      context->Global()
          ->Set(context, V8AtomicString(isolate, kAnnotationHookName), hook)
          .IsNothing()) {
    return;
  }
  if (Disabled("RECORD_REPLAY_DISABLE_REACT_DEVTOOLS")) {
    return;
  }
  RunPageScript(isolate, context, kReactDevtoolsScript,
                "record-replay-react-devtools");
  RunPageScript(isolate, context, kReduxDevtoolsScript,
                "record-replay-redux-devtools");
}

}  // namespace blink
