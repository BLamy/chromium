// Copyright (c) 2021 Record Replay Inc.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The record/replay driver's API, for everything outside V8.
//
// On Chromium 108 this file called exports of Replay's V8 (V8RecordReplay*),
// which looked the driver up and kept what state there was. On 158, V8's port
// keeps to itself (v8/src/replay), so this binds the driver directly. dlsym is
// the engine's own when replaying, and it answers for the same names.

#include "base/record_replay.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <atomic>
#include <sstream>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "base/json/json_writer.h"
#include "base/values.h"

namespace recordreplay {

namespace {

// Every driver function this uses, with its type. All are present in a driver
// built from backend/src/cpp, and any that's missing stays null and its caller
// does nothing.
#define ForEachDriverSymbol(Macro)                                             \
  Macro(RecordReplayAddMetadata, void, (const char*))                          \
  Macro(RecordReplayIsReplaying, bool, ())                                     \
  Macro(RecordReplayGetRecordingId, const char*, ())                           \
  Macro(RecordReplayFinishRecording, void, ())                                 \
  Macro(RecordReplayHadMismatch, bool, ())                                     \
  Macro(RecordReplayAssert, void, (const char*, va_list))                      \
  Macro(RecordReplayAssertBytes, void, (const char*, const void*, size_t))     \
  Macro(RecordReplayAreAssertsDisabled, bool, ())                              \
  Macro(RecordReplayPrint, void, (const char*, va_list))                       \
  Macro(RecordReplayDiagnostic, void, (const char*, va_list))                  \
  Macro(RecordReplayTrace, void, (const char*, va_list))                       \
  Macro(RecordReplayWarning, void,                                             \
        (const char*, const RecordReplayWarningField*, size_t))                \
  Macro(RecordReplayValue, uintptr_t, (const char*, uintptr_t))                \
  Macro(RecordReplayBytes, void, (const char*, void*, size_t))                 \
  Macro(RecordReplayCreateOrderedLock, int, (const char*))                     \
  Macro(RecordReplayOrderedLock, void, (int))                                  \
  Macro(RecordReplayOrderedUnlock, void, (int))                                \
  Macro(RecordReplayInvalidateRecording, void, (const char*, ...))             \
  Macro(RecordReplayNewCheckpoint, void, ())                                   \
  Macro(RecordReplayNewCheckpointFlushed, void, ())                            \
  Macro(RecordReplaySetScriptAlive, void, (bool))                              \
  Macro(RecordReplayNewBookmark, size_t, ())                                   \
  Macro(RecordReplayOnAnnotation, void, (const char*, const char*))            \
  Macro(RecordReplayOnNetworkRequest, void, (const char*, const char*, int))   \
  Macro(RecordReplayOnNetworkRequestEvent, void, (const char*))                \
  Macro(RecordReplayOnNetworkStreamStart, void,                                \
        (const char*, const char*, const char*))                               \
  Macro(RecordReplayOnNetworkStreamData, void,                                 \
        (const char*, size_t, size_t, size_t))                                 \
  Macro(RecordReplayOnNetworkStreamEnd, void, (const char*, size_t))           \
  Macro(RecordReplayBeginPassThroughEvents, void, ())                          \
  Macro(RecordReplayEndPassThroughEvents, void, ())                            \
  Macro(RecordReplayAreEventsPassedThrough, bool, ())                          \
  Macro(RecordReplayBeginDisallowEvents, void, ())                             \
  Macro(RecordReplayBeginDisallowEventsWithLabel, void, (const char*))         \
  Macro(RecordReplayEndDisallowEvents, void, ())                               \
  Macro(RecordReplayAreEventsDisallowed, bool, ())                             \
  Macro(RecordReplayFeatureEnabled, bool, (const char*, const char*))          \
  Macro(RecordReplayHasDisabledFeatures, bool, ())                             \
  Macro(RecordReplayHasDivergedFromRecording, bool, ())                        \
  Macro(RecordReplayAllowSideEffects, bool, ())                                \
  Macro(RecordReplayRegisterPointerWithName, void, (const char*, void*))       \
  Macro(RecordReplayUnregisterPointer, void, (void*))                          \
  Macro(RecordReplayPointerId, int, (void*))                                   \
  Macro(RecordReplayIdPointer, void*, (int))                                   \
  Macro(RecordReplayOnEvent, void, (const char*, bool))                        \
  Macro(RecordReplayOnMouseEvent, void, (const char*, size_t, size_t, bool))   \
  Macro(RecordReplayOnKeyEvent, void, (const char*, const char*, bool))        \
  Macro(RecordReplayOnNavigationEvent, void, (const char*, const char*))       \
  Macro(RecordReplayNewDependencyGraphNode, int, (const char*))                \
  Macro(RecordReplayAddDependencyGraphEdge, void, (int, int, const char*))     \
  Macro(RecordReplayBeginDependencyExecution, void, (int))                     \
  Macro(RecordReplayEndDependencyExecution, void, ())

#define DeclareDriverSymbol(Name, ReturnType, Formals) \
  ReturnType (*g##Name) Formals;
ForEachDriverSymbol(DeclareDriverSymbol)
#undef DeclareDriverSymbol

// 0 before the first look, 1 when this process is neither recording nor
// replaying, 2 when it is.
std::atomic<int> gState{0};

pthread_t gMainThread;

// Nonzero while Replay's own scripts run on the main thread.
int gReplayCodeDepth;

bool gHasAsserts;

// With RECORD_REPLAY_DEPENDENCY_GRAPH_ASSERTS set, a recording process goes
// through the motions of building the graph too, and what it would have added
// is asserted, so a replay that builds a different graph says where.
bool gDependencyGraphAsserts;


int InitializeSlow() {
  // The driver is loaded by main() before anything else when this process is
  // to be recorded (chrome/app/record_replay_main.cc). When replaying, dlsym
  // is the engine's.
  if (!dlsym(RTLD_DEFAULT, "RecordReplayAttach")) {
    gState.store(1, std::memory_order_release);
    return 1;
  }
#define LoadDriverSymbol(Name, ReturnType, Formals) \
  g##Name = reinterpret_cast<ReturnType(*) Formals>(dlsym(RTLD_DEFAULT, #Name));
  ForEachDriverSymbol(LoadDriverSymbol)
#undef LoadDriverSymbol
  gMainThread = pthread_self();
  gHasAsserts = !!getenv("RECORD_REPLAY_ENABLE_ASSERTS");
  gDependencyGraphAsserts = !!getenv("RECORD_REPLAY_DEPENDENCY_GRAPH_ASSERTS");
  gState.store(2, std::memory_order_release);
  return 2;
}

inline bool Active() {
  int state = gState.load(std::memory_order_acquire);
  if (state == 0) [[unlikely]] {
    state = InitializeSlow();
  }
  return state == 2;
}

}  // namespace

void Initialize() {
  Active();
}

bool IsRecordingOrReplaying(const char* feature, const char* subfeature) {
  if (!Active()) {
    return false;
  }
  if (!feature) {
    return true;
  }
  return FeatureEnabled(feature, subfeature);
}

bool IsRecording() {
  return Active() && !gRecordReplayIsReplaying();
}

bool IsReplaying() {
  return Active() && gRecordReplayIsReplaying();
}

char* GetRecordingId() {
  return Active() && gRecordReplayGetRecordingId
             ? const_cast<char*>(gRecordReplayGetRecordingId())
             : nullptr;
}

namespace {

// The main thread's task runner, as only that thread can finish a recording.
base::SingleThreadTaskRunner* gMainThreadTaskRunner;

void DoFinishRecording() {
  Print("DoFinishRecording");
  gRecordReplayFinishRecording();
  Print("RecordingFinished");
  if (gRecordReplayIsReplaying()) {
    // A replay that gets here is at the end of its recording. Whatever is
    // replaying it decides what happens next, and exiting would take it along,
    // so this thread stops here, as every other does when its part of the
    // recording runs out.
    AutoPassThroughEvents pass_through;
    for (;;) {
      sleep(UINT32_MAX);
    }
  }
  _exit(0);
}

// Set when another thread has asked the main thread to finish the recording.
std::atomic<bool> gFinishRecordingRequested{false};

// What wakes the main thread when it's blocked waiting for a sync reply, so
// that it can finish the recording: see MaybeTerminate().
std::atomic<void (*)(void*)> gTerminateCallback{nullptr};
std::atomic<void*> gTerminateCallbackData{nullptr};

}  // namespace

BASE_EXPORT void MaybeFinishRecording();

// Used to make sure we finish recordings on the main thread, even if we're
// blocked in a sync event, as Replay's V8 did on Chromium 108. With a
// callback: the main thread is about to block, and the callback wakes it.
// Without one: the main thread woke for it, and the recording is finished if
// that was asked for.
BASE_EXPORT void MaybeTerminate(void (*callback)(void*), void* data) {
  if (!Active() || !IsMainThread()) {
    return;
  }
  if (callback) {
    gTerminateCallbackData.store(data, std::memory_order_release);
    gTerminateCallback.store(callback, std::memory_order_release);
    return;
  }
  gTerminateCallback.store(nullptr, std::memory_order_release);
  MaybeFinishRecording();
}

// Finishes the recording if it's been asked for, on the main thread, and only
// with events allowed: the driver writes nothing of a recording's end with
// them disallowed. The task that asks for it can be run that way, when it was
// posted from somewhere outside the recording (see TaskQueueImpl's unordered
// tasks), so the task queue also calls this once those have run.
BASE_EXPORT void MaybeFinishRecording() {
  if (!Active() || !IsMainThread() || AreEventsDisallowed() ||
      AreEventsPassedThrough() || HasDivergedFromRecording()) {
    return;
  }
  // Another thread sets this, so when the main thread first sees it set is
  // recorded: a replay finishes where the recording did, and not a task or
  // two to either side.
  if (!gRecordReplayValue(
          "FinishRecordingRequested",
          gFinishRecordingRequested.load(std::memory_order_acquire))) {
    return;
  }
  DoFinishRecording();
}

void SetMainThreadTaskRunner(
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  if (!Active() || gMainThreadTaskRunner) {
    return;
  }
  // Never released: it's needed for as long as the process lives.
  gMainThreadTaskRunner = task_runner.release();
}

void FinishRecording() {
  if (!Active() || !gRecordReplayFinishRecording) {
    return;
  }
  if (IsMainThread()) {
    DoFinishRecording();
  }
  // The driver flushes the recording from the main thread only. A renderer
  // usually ends on its IO thread, when the channel to the browser closes, so
  // this hands over and waits to be ended, as Replay's V8 did on Chromium 108.
  Print("PendingFinishRecording");
  gFinishRecordingRequested.store(true, std::memory_order_release);
  // Wake the main thread if it's blocked waiting for a sync reply.
  if (auto* wake = gTerminateCallback.exchange(nullptr,
                                               std::memory_order_acq_rel)) {
    AutoPassThroughEvents pass_through;
    wake(gTerminateCallbackData.load(std::memory_order_acquire));
  }
  if (!gMainThreadTaskRunner ||
      !gMainThreadTaskRunner->PostTask(FROM_HERE,
                                       base::BindOnce(&MaybeFinishRecording))) {
    // There's no main thread to ask. The recording ends where it was last
    // flushed.
    return;
  }
  sleep(UINT32_MAX);
}

bool HadMismatch() {
  return Active() && gRecordReplayHadMismatch && gRecordReplayHadMismatch();
}

bool HasAsserts() {
  return Active() && gHasAsserts;
}

void Assert(const char* format, ...) {
  if (!Active() || !gRecordReplayAssert) {
    return;
  }
  va_list ap;
  va_start(ap, format);
  gRecordReplayAssert(format, ap);
  va_end(ap);
}

void AssertMaybeEventsDisallowed(const char* format, ...) {
  if (!Active() || !gRecordReplayAssert || AreEventsDisallowed() ||
      HasDivergedFromRecording()) {
    return;
  }
  va_list ap;
  va_start(ap, format);
  gRecordReplayAssert(format, ap);
  va_end(ap);
}

void AssertBytes(const char* why, const void* buf, size_t size) {
  if (Active() && gRecordReplayAssertBytes) {
    gRecordReplayAssertBytes(why, buf, size);
  }
}

bool AreAssertsDisabled() {
  return Active() && gRecordReplayAreAssertsDisabled &&
         gRecordReplayAreAssertsDisabled();
}

void Print(const char* format, ...) {
  if (!Active() || !gRecordReplayPrint) {
    return;
  }
  va_list ap;
  va_start(ap, format);
  gRecordReplayPrint(format, ap);
  va_end(ap);
}

void Diagnostic(const char* format, ...) {
  if (!Active() || !gRecordReplayDiagnostic) {
    return;
  }
  va_list ap;
  va_start(ap, format);
  gRecordReplayDiagnostic(format, ap);
  va_end(ap);
}

void CommandDiagnostic(const char* format, ...) {
  if (!Active() || !gRecordReplayDiagnostic) {
    return;
  }
  va_list ap;
  va_start(ap, format);
  gRecordReplayDiagnostic(format, ap);
  va_end(ap);
}

void CommandDiagnosticTrace(const char* format, ...) {
  if (!Active() || !gRecordReplayDiagnostic) {
    return;
  }
  va_list ap;
  va_start(ap, format);
  gRecordReplayDiagnostic(format, ap);
  va_end(ap);
}

void Warning(const char* kind,
             std::initializer_list<RecordReplayWarningField> fields) {
  Warning(kind, fields.begin(), fields.size());
}

void Warning(const char* kind,
             const RecordReplayWarningField* fields,
             size_t count) {
  if (Active() && gRecordReplayWarning) {
    gRecordReplayWarning(kind, fields, count);
  }
}

void Trace(const char* format, ...) {
  if (!Active() || !gRecordReplayTrace) {
    return;
  }
  va_list ap;
  va_start(ap, format);
  gRecordReplayTrace(format, ap);
  va_end(ap);
}

uintptr_t RecordReplayValue(const char* why, uintptr_t v) {
  return Active() ? gRecordReplayValue(why, v) : v;
}

void OnHTMLParse(const std::string& url) {
  if (!Active() || !IsMainThread() || url.empty() ||
      url.starts_with("about:") ||
      (url.starts_with("chrome") && !url.starts_with("chrome-extension"))) {
    return;
  }
  static bool has_parse = false;
  if (has_parse) {
    return;
  }
  has_parse = true;
  std::string json;
  base::JSONWriter::Write(base::DictValue().Set("uri", url), &json);
  gRecordReplayAddMetadata(json.c_str());
}

uintptr_t RecordReplayValueIfAllowed(const char* why, uintptr_t v) {
  if (!Active() || gRecordReplayAreEventsDisallowed() ||
      gRecordReplayAreEventsPassedThrough() ||
      gRecordReplayHasDivergedFromRecording()) {
    return v;
  }
  return gRecordReplayValue(why, v);
}

void RecordReplayBytes(const char* why, void* buf, size_t size) {
  if (Active()) {
    gRecordReplayBytes(why, buf, size);
  }
}

void RecordReplayString(const char* why, std::string& str) {
  size_t length = RecordReplayValue(why, str.length());
  str.resize(length);
  if (length) {
    RecordReplayBytes(why, &str[0], length);
  }
}

int CreateOrderedLock(const char* name) {
  return Active() ? gRecordReplayCreateOrderedLock(name) : 0;
}

void OrderedLock(int lock) {
  if (Active()) {
    gRecordReplayOrderedLock(lock);
  }
}

void OrderedUnlock(int lock) {
  if (Active()) {
    gRecordReplayOrderedUnlock(lock);
  }
}

void InvalidateRecording(const char* why) {
  if (Active() && gRecordReplayInvalidateRecording) {
    gRecordReplayInvalidateRecording("%s", why);
  }
}

// Set once V8 has attached and taken the first checkpoint, which is where the
// engine starts from. Nothing takes one before, and only the thread the driver
// attached on takes them.
static bool gCheckpointsAllowed;

void OnV8Initialized() {
  if (Active() && IsMainThread()) {
    gCheckpointsAllowed = true;
  }
}

void NewCheckpoint() {
  if (Active() && gCheckpointsAllowed && IsMainThread() &&
      !gRecordReplayAreEventsDisallowed()) {
    gRecordReplayNewCheckpoint();
  }
}

void NewCheckpointFlushed() {
  if (Active() && gCheckpointsAllowed && IsMainThread() &&
      gRecordReplayNewCheckpointFlushed) {
    gRecordReplayNewCheckpointFlushed();
  }
}

void SetScriptAlive(bool alive) {
  if (Active() && gRecordReplaySetScriptAlive) {
    gRecordReplaySetScriptAlive(alive);
  }
}

uint64_t NewBookmark() {
  return Active() && gRecordReplayNewBookmark ? gRecordReplayNewBookmark() : 0;
}

void OnAnnotation(const char* kind, const char* contents) {
  if (Active() && gRecordReplayOnAnnotation) {
    gRecordReplayOnAnnotation(kind, contents);
  }
}

void OnNetworkRequest(const char* id, const char* kind, uint64_t bookmark) {
  if (Active() && gRecordReplayOnNetworkRequest) {
    gRecordReplayOnNetworkRequest(id, kind, static_cast<int>(bookmark));
  }
}

void OnNetworkRequestEvent(const char* id) {
  if (Active() && gRecordReplayOnNetworkRequestEvent) {
    gRecordReplayOnNetworkRequestEvent(id);
  }
}

void OnNetworkStreamStart(const char* id,
                          const char* kind,
                          const char* parentId) {
  if (Active() && gRecordReplayOnNetworkStreamStart) {
    gRecordReplayOnNetworkStreamStart(id, kind, parentId);
  }
}

void OnNetworkStreamData(const char* id,
                         size_t offset,
                         size_t length,
                         uint64_t bookmark) {
  if (Active() && gRecordReplayOnNetworkStreamData) {
    gRecordReplayOnNetworkStreamData(id, offset, length, bookmark);
  }
}

void OnNetworkStreamEnd(const char* id, size_t length) {
  if (Active() && gRecordReplayOnNetworkStreamEnd) {
    gRecordReplayOnNetworkStreamEnd(id, length);
  }
}

void BeginPassThroughEvents() {
  if (Active()) {
    gRecordReplayBeginPassThroughEvents();
  }
}

void EndPassThroughEvents() {
  if (Active()) {
    gRecordReplayEndPassThroughEvents();
  }
}

bool AreEventsPassedThrough(const char* why) {
  return Active() && gRecordReplayAreEventsPassedThrough();
}

void BeginDisallowEvents() {
  if (Active()) {
    gRecordReplayBeginDisallowEvents();
  }
}

void BeginDisallowEventsWithLabel(const char* label) {
  if (Active()) {
    gRecordReplayBeginDisallowEventsWithLabel(label);
  }
}

void EndDisallowEvents() {
  if (Active()) {
    gRecordReplayEndDisallowEvents();
  }
}

bool AreEventsDisallowed(const char* why) {
  return Active() && gRecordReplayAreEventsDisallowed();
}

void EnterReplayCode() {
  if (Active() && IsMainThread()) {
    gReplayCodeDepth++;
  }
}

void ExitReplayCode() {
  if (Active() && IsMainThread()) {
    CHECK(gReplayCodeDepth > 0);
    gReplayCodeDepth--;
  }
}

bool IsInReplayCode(const char* why) {
  return Active() && IsMainThread() && gReplayCodeDepth > 0;
}

bool FeatureEnabled(const char* feature, const char* subfeature) {
  if (!Active() || !gRecordReplayFeatureEnabled) {
    return true;
  }
  return gRecordReplayFeatureEnabled(feature, subfeature);
}

bool HasDisabledFeatures() {
  return Active() && gRecordReplayHasDisabledFeatures &&
         gRecordReplayHasDisabledFeatures();
}

void GetCurrentJSStack(std::string* stackTrace) {
  // V8's port doesn't expose this yet.
}

void BrowserEvent(const char* name, const base::DictValue& info) {
  // The driver takes these through a callback V8's port doesn't register yet.
}

bool HasDivergedFromRecording() {
  return Active() && gRecordReplayHasDivergedFromRecording();
}

bool AllowSideEffects() {
  return !Active() || !gRecordReplayAllowSideEffects ||
         gRecordReplayAllowSideEffects();
}

void RegisterPointer(const char* name, const void* ptr) {
  if (Active() && gRecordReplayRegisterPointerWithName) {
    gRecordReplayRegisterPointerWithName(name, const_cast<void*>(ptr));
  }
}

void UnregisterPointer(const void* ptr) {
  if (Active() && gRecordReplayUnregisterPointer) {
    gRecordReplayUnregisterPointer(const_cast<void*>(ptr));
  }
}

int PointerId(const void* ptr) {
  return Active() && gRecordReplayPointerId
             ? gRecordReplayPointerId(const_cast<void*>(ptr))
             : 0;
}

void* IdPointer(int id) {
  return Active() && gRecordReplayIdPointer ? gRecordReplayIdPointer(id)
                                            : nullptr;
}

void OnEvent(const char* aEvent, bool aBefore) {
  if (Active() && gRecordReplayOnEvent) {
    gRecordReplayOnEvent(aEvent, aBefore);
  }
}

void OnMouseEvent(const char* kind,
                  size_t clientX,
                  size_t clientY,
                  bool synthetic) {
  if (Active() && gRecordReplayOnMouseEvent) {
    gRecordReplayOnMouseEvent(kind, clientX, clientY, synthetic);
  }
}

void OnKeyEvent(const char* kind, const char* key, bool synthetic) {
  if (Active() && gRecordReplayOnKeyEvent) {
    gRecordReplayOnKeyEvent(kind, key, synthetic);
  }
}

void OnNavigationEvent(const char* kind, const char* url) {
  if (Active() && gRecordReplayOnNavigationEvent) {
    gRecordReplayOnNavigationEvent(kind, url);
  }
}

AutoLockMaybeEventsDisallowed::AutoLockMaybeEventsDisallowed(base::Lock& lock)
    : lock_(lock) {
  if (AreEventsDisallowed()) {
    AutoPassThroughEvents pt;
    lock.Acquire();
  } else {
    lock.Acquire();
  }
}

AutoLockMaybeEventsDisallowed::~AutoLockMaybeEventsDisallowed() {
  lock_->Release();
}

AutoUnlockMaybeEventsDisallowed::AutoUnlockMaybeEventsDisallowed(
    base::Lock& lock)
    : lock_(lock) {
  lock_->Release();
}

AutoUnlockMaybeEventsDisallowed::~AutoUnlockMaybeEventsDisallowed() {
  if (AreEventsDisallowed()) {
    AutoPassThroughEvents pt;
    lock_->Acquire();
  } else {
    lock_->Acquire();
  }
}

// The dependency graph is built while replaying only: whoever replays supplies
// the functions that take its nodes, edges and executions, and the driver's
// own, which a recording process has, do nothing. Nothing here may change what
// a replay does next, so no recorded call is made on the way.
static int gDependencyGraphFeature;  // 0 not looked at, 1 off, 2 on.

bool DependencyGraphEnabled() {
  if (!IsMainThread() || (!IsReplaying() && !gDependencyGraphAsserts)) {
    return false;
  }
  if (!gDependencyGraphFeature) {
    gDependencyGraphFeature = FeatureEnabled("dependency-graph") ? 2 : 1;
  }
  return gDependencyGraphFeature == 2;
}

int NewDependencyGraphNode(const char* json) {
  if (!DependencyGraphEnabled()) {
    return 0;
  }
  int id = gRecordReplayNewDependencyGraphNode(json);
  if (gDependencyGraphAsserts) {
    Assert("NewDependencyGraphNode id=%d %s", id, json ? json : "");
  }
  return id;
}

void AddDependencyGraphEdge(int source, int target, const char* json) {
  if (DependencyGraphEnabled()) {
    gRecordReplayAddDependencyGraphEdge(source, target, json);
    if (gDependencyGraphAsserts) {
      Assert("NewDependencyGraphEdge source=%d target=%d %s", source, target,
             json ? json : "");
    }
  }
}

int NewDependencyGraphNodeFrom(int from, const char* edge_kind, const char* json) {
  if (!DependencyGraphEnabled()) {
    return 0;
  }
  int node = NewDependencyGraphNode(json);
  if (from > 0) {
    std::string edge = "{\"kind\":\"";
    edge += edge_kind;
    edge += "\"}";
    AddDependencyGraphEdge(from, node, edge.c_str());
  }
  return node;
}

// The nodes whose executions are open on the main thread, innermost last.
static std::vector<int>* gDependencyExecutionStack;

int DependencyGraphExecutionNode() {
  if (!IsMainThread() || !gDependencyExecutionStack ||
      gDependencyExecutionStack->empty()) {
    return 0;
  }
  return gDependencyExecutionStack->back();
}

void BeginDependencyExecution(int node) {
  if (!DependencyGraphEnabled()) {
    return;
  }
  if (!gDependencyExecutionStack) {
    gDependencyExecutionStack = new std::vector<int>();
  }
  gDependencyExecutionStack->push_back(node);
  gRecordReplayBeginDependencyExecution(node);
  if (gDependencyGraphAsserts) {
    Assert("BeginDependencyExecution id=%d", node);
  }
}

void EndDependencyExecution() {
  // An execution that began is ended even if the graph was since turned off,
  // and one that never began is not.
  if (!IsMainThread() || !gDependencyExecutionStack ||
      gDependencyExecutionStack->empty()) {
    return;
  }
  gDependencyExecutionStack->pop_back();
  gRecordReplayEndDependencyExecution();
  if (gDependencyGraphAsserts) {
    Assert("EndDependencyExecution");
  }
}

static std::string JsonQuote(const char* text) {
  std::string out = "\"";
  for (const char* p = text ? text : ""; *p; p++) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += *p;
    } else if (c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += *p;
    }
  }
  return out + "\"";
}

std::string DependencyGraphJson(
    const char* kind,
    std::initializer_list<std::pair<const char*, std::string>> strings) {
  std::string json = "{\"kind\":" + JsonQuote(kind);
  for (const auto& [key, value] : strings) {
    json += "," + JsonQuote(key) + ":" + JsonQuote(value.c_str());
  }
  return json + "}";
}

AutoMarkerDependencyExecution::AutoMarkerDependencyExecution(const char* reason,
                                                             const char* name) {
  if (DependencyGraphEnabled()) {
    std::string json =
        DependencyGraphJson("marker", {{"reason", reason}, {"name", name}});
    began_ = true;
    BeginDependencyExecution(NewDependencyGraphNode(json.c_str()));
  }
}

AutoMarkerDependencyExecution::~AutoMarkerDependencyExecution() {
  if (began_) {
    EndDependencyExecution();
  }
}

bool IsMainThread() {
  return Active() && pthread_equal(gMainThread, pthread_self());
}

static int gNextMainThreadId = 1;

static bool CheckNewId(const char* name) {
  if (!IsRecordingOrReplaying()) {
    // Don't track anything.
    return false;
  }
  if (HasDivergedFromRecording()) {
    // Everything is allowed when explicitly diverged.
    return true;
  }
  if (AreEventsDisallowed()) {
    // IDs can be created when events are disallowed when our own scripts
    // create URL objects. This would be nice to improve.
    if (!IsInReplayCode()) {
      Warning("NewIdWithEventsDisallowed", {{"name", name}});
    }
    return false;
  }
  Assert("NewId %s", name);
  return true;
}

int NewIdMainThread(const char* name) {
  if (!CheckNewId(name)) {
    return 0;
  }
  if (!IsMainThread()) {
    fprintf(stderr, "NewIdMainThread not main thread: %s\n", name);
    CHECK(IsMainThread());
  }
  return gNextMainThreadId++;
}

static int gNextScriptWrappableId = 1;

int NewScriptWrappableId() {
  if (!IsMainThread() || (!IsReplaying() && !gDependencyGraphAsserts) ||
      AreEventsDisallowed()) {
    return 0;
  }
  return gNextScriptWrappableId++;
}

static std::atomic<int> gNextAnyThreadId{1};

int NewIdAnyThread(const char* name) {
  if (!CheckNewId(name)) {
    return 0;
  }
  return (int)RecordReplayValue("NewId", (uintptr_t)gNextAnyThreadId++);
}

void Crash(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  fprintf(stderr, "\n");
  va_end(args);
  abort();
}

bool AreEventsUnavailable(const char* why) {
  return AreEventsDisallowed(why) || HasDivergedFromRecording();
}

static size_t gMaxOldSpaceMb;

void SetMaxOldSpaceMb(size_t mb) {
  gMaxOldSpaceMb = mb;
}

size_t MaxOldSpaceMb() {
  return gMaxOldSpaceMb;
}

AutoMarkReplayCode::AutoMarkReplayCode() {
  EnterReplayCode();
}

AutoMarkReplayCode::~AutoMarkReplayCode() {
  ExitReplayCode();
}

AutoAssertBufferAllocations::AutoAssertBufferAllocations(
    const char* issueLabel) {}

AutoAssertBufferAllocations::~AutoAssertBufferAllocations() {}

static std::vector<std::string>* gPseudoStack = nullptr;

static std::vector<std::string>* GetPseudoStack() {
  if (!gPseudoStack) {
    gPseudoStack = new std::vector<std::string>();
  }
  return gPseudoStack;
}

size_t PushPseudoStackFrame(const char* name) {
  std::vector<std::string>* pseudoStack = GetPseudoStack();
  pseudoStack->push_back(name);
  return pseudoStack->size();
}

void PopPseudoStackFrame(const char* name, size_t id) {
  CHECK(gPseudoStack != nullptr);
  CHECK(gPseudoStack->size() == id);
  CHECK(gPseudoStack->back() == name);
  gPseudoStack->pop_back();
}

const std::vector<std::string>& ReadPseudoStack() {
  return *GetPseudoStack();
}

std::string ReadPseudoStackString() {
  if (!gPseudoStack) {
    return std::string();
  }
  std::ostringstream out;
  size_t idx = 0;
  for (auto& frame : *gPseudoStack) {
    out << (idx > 0 ? " => " : "") << frame;
    idx++;
  }
  return out.str();
}

}  // namespace recordreplay
