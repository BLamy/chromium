// Copyright 2026 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_UTIL_ORDERED_MUTEX_H_
#define IPCZ_SRC_UTIL_ORDERED_MUTEX_H_

#include "third_party/abseil-cpp/absl/base/thread_annotations.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "util/record_replay.h"

namespace ipcz {

// An absl::Mutex that threads take in the same order when a recorded process
// replays as they did when it was recorded.
//
// ipcz' state is shared between the thread that receives from a transport and
// whichever threads use portals, and what each does next depends on what the
// other got to first. absl::Mutex says nothing to Replay's driver about who
// that was, so each of these has one of the driver's ordered locks around it.
// Outside a recording it's an absl::Mutex and nothing more.
class ABSL_LOCKABLE OrderedMutex {
 public:
  OrderedMutex() : ordered_lock_(recordreplay::CreateOrderedLock("ipcz")) {}
  OrderedMutex(const OrderedMutex&) = delete;
  OrderedMutex& operator=(const OrderedMutex&) = delete;

  void Lock() ABSL_EXCLUSIVE_LOCK_FUNCTION() {
    recordreplay::OrderedLock(ordered_lock_);
    mutex_.Lock();
  }

  void Unlock() ABSL_UNLOCK_FUNCTION() {
    mutex_.Unlock();
    recordreplay::OrderedUnlock(ordered_lock_);
  }

  void AssertHeld() const ABSL_ASSERT_EXCLUSIVE_LOCK() { mutex_.AssertHeld(); }

  // The driver's lock for this mutex, or 0. The same number when a recording
  // replays as when it was made, which this mutex's address isn't.
  int ordered_lock() const { return ordered_lock_; }

 private:
  absl::Mutex mutex_;
  const int ordered_lock_;
};

// absl::MutexLock, for an OrderedMutex.
class ABSL_SCOPED_LOCKABLE OrderedMutexLock {
 public:
  explicit OrderedMutexLock(OrderedMutex* mutex)
      ABSL_EXCLUSIVE_LOCK_FUNCTION(mutex)
      : mutex_(mutex) {
    mutex_->Lock();
  }
  OrderedMutexLock(const OrderedMutexLock&) = delete;
  OrderedMutexLock& operator=(const OrderedMutexLock&) = delete;
  ~OrderedMutexLock() ABSL_UNLOCK_FUNCTION() { mutex_->Unlock(); }

 private:
  OrderedMutex* const mutex_;
};

// absl::ReleasableMutexLock, for an OrderedMutex.
class ABSL_SCOPED_LOCKABLE ReleasableOrderedMutexLock {
 public:
  explicit ReleasableOrderedMutexLock(OrderedMutex* mutex)
      ABSL_EXCLUSIVE_LOCK_FUNCTION(mutex)
      : mutex_(mutex) {
    mutex_->Lock();
  }
  ReleasableOrderedMutexLock(const ReleasableOrderedMutexLock&) = delete;
  ReleasableOrderedMutexLock& operator=(const ReleasableOrderedMutexLock&) =
      delete;
  ~ReleasableOrderedMutexLock() ABSL_UNLOCK_FUNCTION() {
    if (mutex_) {
      mutex_->Unlock();
    }
  }

  void Release() ABSL_UNLOCK_FUNCTION() {
    mutex_->Unlock();
    mutex_ = nullptr;
  }

 private:
  OrderedMutex* mutex_;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_UTIL_ORDERED_MUTEX_H_
