// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_TASK_SEQUENCE_MANAGER_ENQUEUE_ORDER_GENERATOR_H_
#define BASE_TASK_SEQUENCE_MANAGER_ENQUEUE_ORDER_GENERATOR_H_

#include <stdint.h>

#include <atomic>

#include "base/record_replay.h"
#include "base/base_export.h"
#include "base/task/sequence_manager/enqueue_order.h"

namespace base {
namespace sequence_manager {
namespace internal {

// EnqueueOrder can't be created from a raw number in non-test code.
// EnqueueOrderGenerator is used to create it with strictly monotonic guarantee.
class BASE_EXPORT EnqueueOrderGenerator {
 public:
  EnqueueOrderGenerator();
  EnqueueOrderGenerator(const EnqueueOrderGenerator&) = delete;
  EnqueueOrderGenerator& operator=(const EnqueueOrderGenerator&) = delete;
  ~EnqueueOrderGenerator();

  // Can be called from any thread.
  //
  // When recording, the number each caller gets is recorded. Threads post to a
  // thread's queues under one lock per queue, and the thread itself numbers a
  // delayed task that has come due under none, so which of two tasks gets the
  // lower number is a race, and the lower one runs first. A replay hands back
  // the recorded numbers, and keeps its own counter ahead of them for callers
  // that aren't in the recording.
  EnqueueOrder GenerateNext() {
    uint64_t next = std::atomic_fetch_add_explicit(&counter_, uint64_t(1),
                                                   std::memory_order_relaxed);
    if (recordreplay::IsRecordingOrReplaying()) {
      const uint64_t recorded = static_cast<uint64_t>(
          recordreplay::RecordReplayValueIfAllowed("EnqueueOrder", next));
      if (recorded != next) {
        next = recorded;
        uint64_t current = counter_.load(std::memory_order_relaxed);
        while (current <= recorded &&
               !counter_.compare_exchange_weak(current, recorded + 1,
                                               std::memory_order_relaxed)) {
        }
      }
    }
    return EnqueueOrder(next);
  }

 private:
  std::atomic<uint64_t> counter_;
};

}  // namespace internal
}  // namespace sequence_manager
}  // namespace base

#endif  // BASE_TASK_SEQUENCE_MANAGER_ENQUEUE_ORDER_GENERATOR_H_
