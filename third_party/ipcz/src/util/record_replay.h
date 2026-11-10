// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_UTIL_RECORD_REPLAY_H_
#define IPCZ_SRC_UTIL_RECORD_REPLAY_H_

// What ipcz needs of Replay's record/replay driver, looked up by name as ipcz
// doesn't depend on //base.
//
// Two nodes share memory: link state, reference counts, ID counters and block
// allocators, all updated by both with atomics, and the data of larger parcels.
// When a recorded process replays, the other node isn't there, and the memory
// is its own. So everything this node reads from shared memory is treated as
// input: recorded when recording, and taken from the recording when replaying.
// What it writes there is written as usual, and nothing reads it back.

#include <cstddef>
#include <cstdint>

#if defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace ipcz::recordreplay {

namespace internal {

struct Driver {
  bool active = false;
  bool (*is_replaying)() = nullptr;
  bool (*are_events_disallowed)() = nullptr;
  bool (*are_events_passed_through)() = nullptr;
  bool (*has_diverged)() = nullptr;
  uintptr_t (*value)(const char*, uintptr_t) = nullptr;
  void (*bytes)(const char*, void*, size_t) = nullptr;
  void (*begin_pass_through)() = nullptr;
  void (*end_pass_through)() = nullptr;
  int (*create_ordered_lock)(const char*) = nullptr;
  void (*ordered_lock)(int) = nullptr;
  void (*ordered_unlock)(int) = nullptr;
};

inline const Driver& GetDriver() {
  static const Driver driver = [] {
    Driver d;
#if defined(__APPLE__)
    if (!dlsym(RTLD_DEFAULT, "RecordReplayAttach")) {
      return d;
    }
    d.is_replaying =
        reinterpret_cast<bool (*)()>(dlsym(RTLD_DEFAULT, "RecordReplayIsReplaying"));
    d.are_events_disallowed = reinterpret_cast<bool (*)()>(
        dlsym(RTLD_DEFAULT, "RecordReplayAreEventsDisallowed"));
    d.are_events_passed_through = reinterpret_cast<bool (*)()>(
        dlsym(RTLD_DEFAULT, "RecordReplayAreEventsPassedThrough"));
    d.has_diverged = reinterpret_cast<bool (*)()>(
        dlsym(RTLD_DEFAULT, "RecordReplayHasDivergedFromRecording"));
    d.value = reinterpret_cast<uintptr_t (*)(const char*, uintptr_t)>(
        dlsym(RTLD_DEFAULT, "RecordReplayValue"));
    d.bytes = reinterpret_cast<void (*)(const char*, void*, size_t)>(
        dlsym(RTLD_DEFAULT, "RecordReplayBytes"));
    d.begin_pass_through = reinterpret_cast<void (*)()>(
        dlsym(RTLD_DEFAULT, "RecordReplayBeginPassThroughEvents"));
    d.end_pass_through = reinterpret_cast<void (*)()>(
        dlsym(RTLD_DEFAULT, "RecordReplayEndPassThroughEvents"));
    d.create_ordered_lock = reinterpret_cast<int (*)(const char*)>(
        dlsym(RTLD_DEFAULT, "RecordReplayCreateOrderedLock"));
    d.ordered_lock = reinterpret_cast<void (*)(int)>(
        dlsym(RTLD_DEFAULT, "RecordReplayOrderedLock"));
    d.ordered_unlock = reinterpret_cast<void (*)(int)>(
        dlsym(RTLD_DEFAULT, "RecordReplayOrderedUnlock"));
    d.active = d.begin_pass_through && d.end_pass_through &&
               d.create_ordered_lock && d.ordered_lock && d.ordered_unlock &&
               d.is_replaying && d.are_events_disallowed &&
               d.are_events_passed_through && d.has_diverged && d.value &&
               d.bytes;
#endif
    return d;
  }();
  return driver;
}

}  // namespace internal

// Whether this process is being recorded or replayed, and this thread's calls
// are part of the recording right now.
inline bool IsRecordingOrReplaying() {
  const internal::Driver& d = internal::GetDriver();
  return d.active && !d.are_events_disallowed() &&
         !d.are_events_passed_through() && !d.has_diverged();
}

inline bool IsReplaying() {
  return IsRecordingOrReplaying() && internal::GetDriver().is_replaying();
}

// `value` when recording, and what it was then when replaying.
inline uintptr_t Value(const char* why, uintptr_t value) {
  return IsRecordingOrReplaying() ? internal::GetDriver().value(why, value)
                                  : value;
}

// Leaves `buffer` as it is when recording, and fills it with what it held then
// when replaying.
inline void Bytes(const char* why, void* buffer, size_t size) {
  if (IsRecordingOrReplaying()) {
    internal::GetDriver().bytes(why, buffer, size);
  }
}

// One of the driver's ordered locks, or 0 for none: outside a recording, and
// for anything made while this thread's calls aren't part of one.
inline int CreateOrderedLock(const char* name) {
  return IsRecordingOrReplaying()
             ? internal::GetDriver().create_ordered_lock(name)
             : 0;
}

// The driver leaves these alone while this thread's calls aren't part of the
// recording.
inline void OrderedLock(int lock) {
  if (lock) {
    internal::GetDriver().ordered_lock(lock);
  }
}

inline void OrderedUnlock(int lock) {
  if (lock) {
    internal::GetDriver().ordered_unlock(lock);
  }
}

// While one of these is alive, what this thread does isn't in the recording
// and its locks aren't ordered. For something a replay skips altogether, in
// favor of what it recorded as the result.
class AutoPassThroughEvents {
 public:
  AutoPassThroughEvents() : active_(internal::GetDriver().active) {
    if (active_) {
      internal::GetDriver().begin_pass_through();
    }
  }
  ~AutoPassThroughEvents() {
    if (active_) {
      internal::GetDriver().end_pass_through();
    }
  }
  AutoPassThroughEvents(const AutoPassThroughEvents&) = delete;
  AutoPassThroughEvents& operator=(const AutoPassThroughEvents&) = delete;

 private:
  const bool active_;
};

namespace internal {
inline int gAtomicsLock = 0;
}  // namespace internal

// Makes the lock AutoOrderedAtomics takes. Called when the first Node is made,
// which is on one thread, before any other uses ipcz.
inline void InitializeAtomicsLock() {
  if (!internal::gAtomicsLock) {
    internal::gAtomicsLock = CreateOrderedLock("ipcz atomics");
  }
}

// Orders one operation on an atomic that threads signal each other with:
// reference counts, where the thread that drops the last reference is the one
// that destroys the object and sends what that sends, and the few flags and
// counters ipcz keeps outside its mutexes. One lock serves all of them.
class AutoOrderedAtomics {
 public:
  AutoOrderedAtomics() : lock_(internal::gAtomicsLock) { OrderedLock(lock_); }
  ~AutoOrderedAtomics() { OrderedUnlock(lock_); }
  AutoOrderedAtomics(const AutoOrderedAtomics&) = delete;
  AutoOrderedAtomics& operator=(const AutoOrderedAtomics&) = delete;

 private:
  const int lock_;
};

}  // namespace ipcz::recordreplay

#endif  // IPCZ_SRC_UTIL_RECORD_REPLAY_H_
