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

#include "third_party/blink/renderer/bindings/core/v8/record_replay_network.h"
#include "third_party/blink/renderer/core/inspector/record_replay_dom.h"
#include "third_party/blink/renderer/core/execution_context/agent.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "third_party/blink/renderer/platform/crypto.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/scheduler/public/event_loop.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "base/record_replay.h"
#include "third_party/blink/renderer/core/dom/node.h"
#include "third_party/blink/renderer/platform/bindings/script_wrappable.h"
#include "third_party/blink/renderer/platform/bindings/v8_binding.h"
#include "third_party/blink/renderer/platform/bindings/v8_dom_wrapper.h"
#include "third_party/blink/renderer/platform/bindings/wrapper_type_info.h"
#include "v8/include/v8.h"

// V8's record/replay port keeps to itself, and exports these for its embedder
// (v8/src/replay/replay-glue.cc).
extern "C" void V8RecordReplaySetPageCommandHandler(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    v8::Local<v8::Function> function);
extern "C" uint64_t V8RecordReplayProgressCounter();
extern "C" const char* V8RecordReplayPendingPrivilegedEvaluation(size_t index);
extern "C" void V8RecordReplayBeginReplayCode(const char* label);
extern "C" void V8RecordReplayEndReplayCode();
extern "C" void V8RecordReplaySetDependencyGraphFunctions(
    bool (*enabled)(),
    int (*new_node)(const char* json),
    void (*add_edge)(int source, int target, const char* json),
    void (*begin_execution)(int node),
    void (*end_execution)(),
    int (*execution_node)());
extern "C" void V8RecordReplaySetApiObjectIdCallback(
    int (*callback)(v8::Local<v8::Object> object));
extern "C" void V8RecordReplaySetNewScriptCallback(
    void (*callback)(v8::Isolate* isolate,
                     int script_id,
                     const char* url,
                     const char* source_map_url));
extern "C" char* V8RecordReplayScriptSource(int script_id);
extern "C" int V8RecordReplayObjectId(v8::Isolate* isolate,
                                      v8::Local<v8::Value> value,
                                      bool allow_create);

namespace blink {

namespace {

const char kArgumentsName[] = "__RECORD_REPLAY_ARGUMENTS__";
const char kApiName[] = "__RECORD_REPLAY__";
const char kInternalScriptName[] = "record-replay-internal";
const char kEternalStateName[] = "__RECORD_REPLAY_ETERNAL_STATE__";

// What a replay's own code in the page is given: upstream's
// __RECORD_REPLAY_ARGUMENTS__, as far as whoever replays uses it. That's code
// evaluated with Target.evaluatePrivileged, and scripts it registers to run in
// every new root frame before the page's own. This runs with events
// disallowed, on both sides, and isn't instrumented.
const char kPageApiScript[] = R""""(
(() => {
"use strict";
const args = __RECORD_REPLAY_ARGUMENTS__;

// getPersistentId and checkPersistentId are V8's: IDs for objects, the same for
// as long as the object lives, which are the ones the dependency graph names
// DOM objects and promises by.
args.internal = {};

// Functions called with each of the page's new scripts that names a source
// map: (scriptId, sourceURL, sourceMapURL).
const newScriptHandlers = [];
args.addNewScriptHandler = handler => {
  newScriptHandlers.push(handler);
};
args.internal.onNewScript = (scriptId, sourceURL, sourceMapURL) => {
  for (const handler of newScriptHandlers) {
    handler(scriptId, sourceURL, sourceMapURL);
  }
};

const commands = {
  "Target.evaluatePrivileged": ({ expression }) => {
    const result = eval(expression);
    return { result };
  },
};

// What was asked for before there was a page, which is how whoever replays
// sets itself up: it registers its root frame scripts this way.
for (const paramsText of args.takePendingEvaluations()) {
  try {
    commands["Target.evaluatePrivileged"](JSON.parse(paramsText));
  } catch (e) {
    args.log(`Error: an early evaluation threw ${e && e.stack || e}`);
  }
}

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

// One object for as long as the process lives, which every root frame is
// given: what whoever replays keeps across navigations.
v8::Local<v8::Object> EternalState(v8::Isolate* isolate) {
  static v8::Eternal<v8::Object>* state = nullptr;
  if (!state) {
    state = new v8::Eternal<v8::Object>(isolate, v8::Object::New(isolate));
  }
  return state->Get(isolate);
}

void ArgsLog(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::String::Utf8Value text(args.GetIsolate(), args[0]);
  recordreplay::Print("%s", *text ? *text : "");
}

// Upstream's replay-assets/replay_sourcemap_handler.js, unchanged: fetches the
// source map of each new script, and the original sources the map doesn't
// carry, and writes them into the recordings folder. It runs as part of the
// page on both sides, so what it fetches is in the recording.
const char kSourceMapScript[] = R"RRSM(// Script which sets a handler for collecting source maps from scripts in the
// recording. Runs when recording/replaying if source map collection is enabled.
(() => {

// Avoid monkey patching.
const { fetch, URL, Error } = window;
const DateNow = Date.now;

const {
  log,
  warning,
  getRecordingId,
  sha256DigestHex,
  writeToRecordingDirectory,
  addRecordingEvent,
  addNewScriptHandler,
  getScriptSource,
  recordingDirectoryFileExists,
  readFromRecordingDirectory,
  getRecordingFilePath,
  RECORD_REPLAY_DISABLE_SOURCEMAP_CACHE,
} = __RECORD_REPLAY_ARGUMENTS__;

const fetchPromiseCache = {};

async function fetchText(url) {
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`Fetching ${url} failed with status code ${response.status} (${response.statusText})`);
  }
  return await response.text();
}

// Provide a cache for urls, salted with the supplied hash.  Practically, this
// means if the script content changes at the url, we will re-download the resource.
async function fetchTextWithCache(url, hash) {
  const key = `${url}:${hash}`;
  if (fetchPromiseCache[key] && !RECORD_REPLAY_DISABLE_SOURCEMAP_CACHE) {
    // Return past or on-going work item.
    return fetchPromiseCache[key];
  }

  log(`[sourcemaps] Fetching sourcemap resource ${key}`);

  const resPromise = fetchText(url);
  fetchPromiseCache[key] = resPromise;
  return resPromise;
}

addNewScriptHandler(async (scriptId, sourceURL, relativeSourceMapURL) => {
  try {
  if (!relativeSourceMapURL || relativeSourceMapURL.startsWith("data:"))
    return;

  const recordingId = getRecordingId();
  if (!recordingId) {
    // The recording has been invalidated.
    return;
  }

  const urls = getSourceMapURLs(sourceURL, relativeSourceMapURL);
  if (!urls)
    return;

  // Yield so full-source SHA256 runs after sync script registration, not under ProcessCompileEvent.
  await Promise.resolve();

  const scriptSource = getScriptSource(scriptId);
  const generatedScriptHash = sha256DigestHex(scriptSource);

  const { sourceMapURL, sourceMapBaseURL } = urls;

  let sourceMap;
  try {
    sourceMap = await fetchTextWithCache(sourceMapURL, generatedScriptHash);
  } catch (err) {
    log(`[RuntimeError][sourcemaps] Failed to read sourcemap ${sourceMapURL}: ${err.message}`);
  }
  if (!sourceMap) {
    // Download failed or nothing there.
    return;
  }

  const id = generatedScriptHash;
  const name = `sourcemap-${id}.map`;
  const lookupName = `sourcemap-${id}.lookup`;

  let sources;
  if (recordingDirectoryFileExists(name) && recordingDirectoryFileExists(lookupName)) {
    try {
      sources = JSON.parse(readFromRecordingDirectory(lookupName));
    } catch (err) {
      log(`[RuntimeError][sourcemaps] Failed to load sourcemaps from file: ${lookupName} - ${err.message}`);
    }
  }

  if (!sources) {
    // Sources changed or did not exist.
    writeToRecordingDirectory(name, sourceMap);

    sources = collectUnresolvedSourceMapResources(sourceMap, sourceMapURL);
    writeToRecordingDirectory(lookupName, JSON.stringify(sources));
  }

  log(`[sourcemaps] Wrote sourcemap to file. Found ${sources.length} unresolved sources for "${sourceMapURL}". Downloading...`);

  addRecordingEvent(JSON.stringify({
    kind: "sourcemapAdded",
    path: getRecordingFilePath(name),
    recordingId,
    id,
    url: sourceMapURL,
    baseURL: sourceMapBaseURL,
    targetContentHash: `sha256:${generatedScriptHash}`,
    targetURLHash: sourceURL ? makeAPIHash(sourceURL) : undefined,
    targetMapURLHash: makeAPIHash(sourceMapURL),
    timestamp: DateNow(),
  }));

  for (const { offset, url } of sources) {
    let sourceContent;
    try {
      sourceContent = await fetchTextWithCache(url, generatedScriptHash);
    } catch (err) {
      log(`[RuntimeError][sourcemaps] Failed to read original source ${url}: ${err.message}`);
    }
    if (!sourceContent) {
      // Download failed or nothing there.
      continue;
    }
    const hash = sha256DigestHex(sourceContent);
    const name = `source-${hash}`;

    if (!recordingDirectoryFileExists(name)) {
      writeToRecordingDirectory(name, sourceContent);
    }
    addRecordingEvent(JSON.stringify({
      kind: "originalSourceAdded",
      path: getRecordingFilePath(name),
      recordingId,
      parentId: id,
      parentOffset: offset,
      timestamp: DateNow(),
    }));
  }
  log(`[sourcemaps] Finished downloading ${sources.length} sources for "${sourceMapURL}".`);
  } catch (err) {
    warning("SourceMapCollectionException", { error: `${err?.stack || err}` });
  }
});

function makeAPIHash(content) {
  assert(typeof content === "string");
  const digestHex = sha256DigestHex(content);
  return "sha256:" + digestHex;
}

function collectUnresolvedSourceMapResources(mapText, mapURL) {
  let obj;
  let sourceOffset = 0;

  function logError(msg) {
    log(`[RuntimeError][sourcemaps] ${msg} (${mapURL}:${sourceOffset})`);
  }

  try {
    obj = JSON.parse(mapText);
    if (typeof obj !== "object" || !obj) {
      return [];
    }
  } catch (err) {
    logError(`Exception parsing sourcemap JSON (${mapURL}): ${err?.message || err}`);
    return [];
  }

  const unresolvedSources = [];
  if (obj.version !== 3) {
    logError("Invalid sourcemap version: " + obj.version);
    return [];
  }

  if (obj.sources != null) {
    const { sourceRoot, sources, sourcesContent } = obj;

    if (Array.isArray(sources)) {
      for (let i = 0; i < sources.length; i++) {
        const offset = sourceOffset++;

        if (
          !Array.isArray(sourcesContent) ||
          typeof sourcesContent[i] !== "string"
        ) {
          let url = sources[i];
          if (typeof sourceRoot === "string" && sourceRoot) {
            url = sourceRoot.replace(/\/?/, "/") + url;
          }
          let sourceURL;
          try {
            sourceURL = new URL(url, mapURL).toString();
          } catch {
            logError("Unable to compute original source URL: " + url);
            continue;
          }

          unresolvedSources.push({
            offset,
            url: sourceURL,
          });
        }
      }
    } else {
      logError("Invalid sourcemap sources list");
    }
  }

  return unresolvedSources;
}

function assert(v, msg = "") {
  if (!v) {
    const m = `Assertion failed when handling command (${msg})`;
    log(`[RuntimeError] ${m} - ${Error().stack}`);
    throw new Error(m);
  }
}

function getSourceMapURLs(sourceURL, relativeSourceMapURL) {
  let sourceBaseURL;
  if (typeof sourceURL === "string" && isValidBaseURL(sourceURL)) {
    sourceBaseURL = sourceURL;
  } else if (window?.location?.href && isValidBaseURL(window?.location?.href)) {
    sourceBaseURL = window.location.href;
  }

  let sourceMapURL;
  try {
    sourceMapURL = new URL(relativeSourceMapURL, sourceBaseURL).toString();
  } catch (err) {
    log("Failed to process sourcemap url: " + err.message);
    return null;
  }

  // If the map was a data: URL or something along those lines, we want
  // to resolve paths in the map relative to the overall base.
  const sourceMapBaseURL =
    isValidBaseURL(sourceMapURL) ? sourceMapURL : sourceBaseURL;

  return { sourceMapURL, sourceMapBaseURL };
}

function isValidBaseURL(url) {
  try {
    new URL("", url);
    return true;
  } catch {
    return false;
  }
}

})();
)RRSM";

std::string RecordingDirectory() {
  if (const char* directory = getenv("RECORD_REPLAY_DIRECTORY")) {
    return directory;
  }
  const char* home = getenv("HOME");
  return std::string(home ? home : "") + "/.replay";
}

std::string RecordingPath(v8::Isolate* isolate, v8::Local<v8::Value> name) {
  v8::String::Utf8Value text(isolate, name);
  return RecordingDirectory() + "/" + (*text ? *text : "");
}

void ReturnText(const v8::FunctionCallbackInfo<v8::Value>& args,
                const std::string& text) {
  v8::Local<v8::String> string;
  if (v8::String::NewFromUtf8(args.GetIsolate(), text.data(),
                              v8::NewStringType::kNormal,
                              static_cast<int>(text.size()))
          .ToLocal(&string)) {
    args.GetReturnValue().Set(string);
  }
}

void ArgsGetRecordingId(const v8::FunctionCallbackInfo<v8::Value>& args) {
  // What the page's script does next depends on this, and whoever replays
  // doesn't have the ID the recording was made under: the answer is recorded.
  char id[128] = {};
  if (!recordreplay::IsReplaying()) {
    if (const char* recording_id = recordreplay::GetRecordingId()) {
      strncpy(id, recording_id, sizeof(id) - 1);
    }
  }
  recordreplay::RecordReplayBytes("GetRecordingId", id, sizeof(id));
  if (id[0]) {
    ReturnText(args, id);
  } else {
    args.GetReturnValue().SetNull();
  }
}

void ArgsSha256DigestHex(const v8::FunctionCallbackInfo<v8::Value>& args) {
  char hex[65] = {};
  if (!recordreplay::IsReplaying()) {
    v8::String::Utf8Value content(args.GetIsolate(), args[0]);
    DigestValue digest;
    if (*content &&
        ComputeDigest(kHashAlgorithmSha256,
                      base::as_bytes(base::span<const char>(
                          *content, static_cast<size_t>(content.length()))),
                      digest)) {
      static const char kDigits[] = "0123456789abcdef";
      for (size_t i = 0; i < 32 && i < digest.size(); i++) {
        hex[i * 2] = kDigits[digest[i] >> 4];
        hex[i * 2 + 1] = kDigits[digest[i] & 15];
      }
    }
  }
  // What's hashed can differ when replaying, when a script's source was
  // replaced, so the answer is the recorded one.
  recordreplay::RecordReplayBytes("SHA256DigestHex", hex, sizeof(hex));
  ReturnText(args, hex);
}

void ArgsGetRecordingFilePath(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ReturnText(args, RecordingPath(args.GetIsolate(), args[0]));
}

void ArgsRecordingDirectoryFileExists(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  std::ifstream stream(RecordingPath(args.GetIsolate(), args[0]));
  args.GetReturnValue().Set(stream.good());
}

void ArgsReadFromRecordingDirectory(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  std::ifstream stream(RecordingPath(args.GetIsolate(), args[0]));
  std::stringstream data;
  data << stream.rdbuf();
  ReturnText(args, data.str());
}

void ArgsWriteToRecordingDirectory(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  std::string path = RecordingPath(args.GetIsolate(), args[0]);
  v8::String::Utf8Value content(args.GetIsolate(), args[1]);
  std::ofstream stream(path);
  if (*content) {
    stream.write(*content, content.length());
  }
  stream.close();
  ReturnText(args, path);
}

void ArgsAddRecordingEvent(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::String::Utf8Value content(args.GetIsolate(), args[0]);
  std::ofstream stream(RecordingDirectory() + "/recordings.log",
                       std::ofstream::app);
  stream << (*content ? *content : "") << "\n";
}

void ArgsGetScriptSource(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::String::Utf8Value id(args.GetIsolate(), args[0]);
  char* source = *id ? V8RecordReplayScriptSource(atoi(*id)) : nullptr;
  ReturnText(args, source ? source : "");
  free(source);
}

// A new script that names a source map, for the root frame's handlers. V8
// says so while it's still compiling the script, so they run afterwards.
void OnNewScriptWithSourceMap(v8::Isolate* isolate,
                              int script_id,
                              const char* url,
                              const char* source_map_url) {
  LocalFrame* frame = RecordReplayDomRootFrame();
  if (!frame || !frame->DomWindow() || !IsMainThread()) {
    return;
  }
  frame->DomWindow()->GetAgent()->event_loop()->EnqueueMicrotask(BindOnce(
      [](LocalFrame* frame, int script_id, const std::string& url,
         const std::string& source_map_url) {
        ScriptState* script_state = RecordReplayMainWorldIfInitialized(frame);
        if (!script_state) {
          return;
        }
        ScriptState::Scope scope(script_state);
        v8::Isolate* isolate = script_state->GetIsolate();
        v8::Local<v8::Context> context = script_state->GetContext();
        v8::TryCatch try_catch(isolate);
        v8::Local<v8::Value> args, internal, handler;
        if (!context->Global()
                 ->Get(context, V8AtomicString(isolate, kArgumentsName))
                 .ToLocal(&args) ||
            !args->IsObject() ||
            !args.As<v8::Object>()
                 ->Get(context, V8AtomicString(isolate, "internal"))
                 .ToLocal(&internal) ||
            !internal->IsObject() ||
            !internal.As<v8::Object>()
                 ->Get(context, V8AtomicString(isolate, "onNewScript"))
                 .ToLocal(&handler) ||
            !handler->IsFunction()) {
          return;
        }
        v8::Local<v8::Value> call_args[] = {
            V8String(isolate, String::Number(script_id)),
            V8String(isolate, String::FromUtf8(base::as_byte_span(url))),
            V8String(isolate, String::FromUtf8(base::as_byte_span(source_map_url)))};
        std::ignore = handler.As<v8::Function>()->Call(
            context, v8::Undefined(isolate), 3, call_args);
      },
      WrapWeakPersistent(frame), script_id, std::string(url ? url : ""),
      std::string(source_map_url ? source_map_url : "")));
}

// The ID a DOM object was made with, which is the same every time a recording
// is replayed. V8 asks for it before giving an object an ID of its own.
int RecordReplayApiObjectId(v8::Local<v8::Object> object) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  if (!V8DOMWrapper::IsWrapper(isolate, object)) {
    return 0;
  }
  return ToAnyScriptWrappable(isolate, object)->RecordReplayId();
}

void ArgsGetPersistentId(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  int id = args.Length() ? V8RecordReplayObjectId(isolate, args[0], true) : 0;
  args.GetReturnValue().Set(v8::Integer::New(isolate, id));
}

void ArgsCheckPersistentId(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  int id = args.Length() ? V8RecordReplayObjectId(isolate, args[0], false) : 0;
  args.GetReturnValue().Set(v8::Integer::New(isolate, id));
}

// What V8 is given once, with the first page: the functions through which
// promises go into the dependency graph, the IDs of DOM objects, and the
// commands answered here.
void RecordReplayInitializeOnce() {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;
  V8RecordReplaySetDependencyGraphFunctions(
      recordreplay::DependencyGraphEnabled,
      recordreplay::NewDependencyGraphNode,
      recordreplay::AddDependencyGraphEdge,
      recordreplay::BeginDependencyExecution,
      recordreplay::EndDependencyExecution,
      recordreplay::DependencyGraphExecutionNode);
  V8RecordReplaySetApiObjectIdCallback(RecordReplayApiObjectId);
  RecordReplayRegisterNetworkCommands();
  RecordReplayRegisterDomCommands();
  V8RecordReplaySetNewScriptCallback(OnNewScriptWithSourceMap);
}

void ArgsGetProgressCounter(const v8::FunctionCallbackInfo<v8::Value>& args) {
  args.GetReturnValue().Set(v8::Number::New(
      args.GetIsolate(), static_cast<double>(V8RecordReplayProgressCounter())));
}

// A hook the replay put into the program's code marks what it does as its own.
void ArgsBeginReplayCode(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::String::Utf8Value label(args.GetIsolate(), args[0]);
  V8RecordReplayBeginReplayCode(*label ? *label : "ReplayCode");
}

void ArgsEndReplayCode(const v8::FunctionCallbackInfo<v8::Value>& args) {
  V8RecordReplayEndReplayCode();
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

// Each is run once, by the first root frame there is.
void ArgsTakePendingEvaluations(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  static size_t taken = 0;
  v8::Isolate* isolate = args.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Array> pending = v8::Array::New(isolate);
  uint32_t count = 0;
  while (const char* params = V8RecordReplayPendingPrivilegedEvaluation(taken)) {
    taken++;
    if (pending->Set(context, count++, V8String(isolate, params)).IsNothing()) {
      break;
    }
  }
  args.GetReturnValue().Set(pending);
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
    v8::String::Utf8Value error(isolate, try_catch.Exception());
    v8::Local<v8::Value> stack;
    std::string where;
    if (try_catch.StackTrace(context).ToLocal(&stack) && stack->IsString()) {
      v8::String::Utf8Value text(isolate, stack);
      if (*text) {
        where = *text;
      }
    }
    recordreplay::Print("[RuntimeError] %s didn't run: %s %.600s (source starts %.80s)",
                        name, *error ? *error : "", where.c_str(),
                        source_chars);
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
      !SetNative(isolate, context, args, "getRecordingId",
                 ArgsGetRecordingId) ||
      !SetNative(isolate, context, args, "sha256DigestHex",
                 ArgsSha256DigestHex) ||
      !SetNative(isolate, context, args, "getRecordingFilePath",
                 ArgsGetRecordingFilePath) ||
      !SetNative(isolate, context, args, "recordingDirectoryFileExists",
                 ArgsRecordingDirectoryFileExists) ||
      !SetNative(isolate, context, args, "readFromRecordingDirectory",
                 ArgsReadFromRecordingDirectory) ||
      !SetNative(isolate, context, args, "writeToRecordingDirectory",
                 ArgsWriteToRecordingDirectory) ||
      !SetNative(isolate, context, args, "addRecordingEvent",
                 ArgsAddRecordingEvent) ||
      !SetNative(isolate, context, args, "getScriptSource",
                 ArgsGetScriptSource) ||
      !SetNative(isolate, context, args, "getPersistentId",
                 ArgsGetPersistentId) ||
      !SetNative(isolate, context, args, "checkPersistentId",
                 ArgsCheckPersistentId) ||
      !SetNative(isolate, context, args, "registerRootFrameScript",
                 ArgsRegisterRootFrameScript) ||
      !SetNative(isolate, context, args, "beginReplayCode",
                 ArgsBeginReplayCode) ||
      !SetNative(isolate, context, args, "endReplayCode", ArgsEndReplayCode) ||
      !SetNative(isolate, context, args, "setCommandCallback",
                 ArgsSetCommandCallback) ||
      !SetNative(isolate, context, args, "takePendingEvaluations",
                 ArgsTakePendingEvaluations) ||
      !SetHidden(isolate, context, context->Global(), kArgumentsName, args) ||
      !SetHidden(isolate, context, context->Global(), kEternalStateName,
                 EternalState(isolate)) ||
      !SetHidden(isolate, context, context->Global(), kApiName, api)) {
    return;
  }
  if (!RunPageScript(isolate, context, kPageApiScript, kInternalScriptName)) {
    return;
  }
  if (!Disabled("RECORD_REPLAY_DISABLE_SOURCEMAP_COLLECTION")) {
    RunPageScript(isolate, context, kSourceMapScript, kInternalScriptName);
  }
  // By index: a script may register another.
  for (size_t i = 0; i < RootFrameScripts().size(); i++) {
    std::string script = RootFrameScripts()[i];
    RunPageScript(isolate, context, script.c_str(), kInternalScriptName);
  }
}

void RecordReplayInstallPageScripts(v8::Isolate* isolate,
                                    v8::Local<v8::Context> context) {
  if (recordreplay::IsRecordingOrReplaying()) {
    RecordReplayInitializeOnce();
  }
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

int RecordReplayOnDOMMutation(Node& target, const char* type) {
  if (!recordreplay::DependencyGraphEnabled()) {
    return -1;
  }
  std::string json = "{\"kind\":\"domMutation\",\"mutationType\":\"";
  json += type;
  json += "\",\"mutationNode\":" + std::to_string(target.RecordReplayId()) +
          "}";
  return recordreplay::NewDependencyGraphNode(json.c_str());
}

}  // namespace blink
