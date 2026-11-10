// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/record_replay.h"
#include "ipcz/ref_counted_fragment.h"

#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {

RefCountedFragment::RefCountedFragment() = default;

void RefCountedFragment::AddRef() {
  ref_count_.fetch_add(1, std::memory_order_relaxed);
}

int32_t RefCountedFragment::ReleaseRef() {
  // The other node may hold references too, so what this sees is recorded.
  return static_cast<int32_t>(recordreplay::Value(
      "ipcz RefCountedFragment::ReleaseRef",
      static_cast<uintptr_t>(static_cast<uint32_t>(
          ref_count_.fetch_sub(1, std::memory_order_acq_rel)))));
}

}  // namespace ipcz
