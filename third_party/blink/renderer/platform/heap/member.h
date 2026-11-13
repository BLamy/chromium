// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_HEAP_MEMBER_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_HEAP_MEMBER_H_
#include "base/check_op.h"
#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/bindings/script_wrappable.h"

#include "base/check.h"
#include "base/record_replay.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/heap/thread_state_storage.h"
#include "third_party/blink/renderer/platform/heap/write_barrier.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"
#include "third_party/blink/renderer/platform/wtf/construct_traits.h"
#include "third_party/blink/renderer/platform/wtf/hash_functions.h"
#include "third_party/blink/renderer/platform/wtf/hash_traits.h"
#include "third_party/blink/renderer/platform/wtf/type_traits.h"
#include "v8/include/cppgc/member.h"  // IWYU pragma: export
#include "v8/include/cppgc/tagged-member.h"

namespace blink {

template <typename T>
using Member = cppgc::Member<T>;

template <typename T>
using WeakMember = cppgc::WeakMember<T>;

template <typename T>
using UntracedMember = cppgc::UntracedMember<T>;

namespace subtle {

template <typename T>
using UncompressedMember = cppgc::subtle::UncompressedMember<T>;

template <typename T, typename Tag1, typename Tag2>
using TaggedUncompressedMember =
    cppgc::subtle::TaggedUncompressedMember<T, Tag1, Tag2>;
}

template <typename T>
inline bool IsHashTableDeletedValue(const Member<T>& m) {
  return m == cppgc::kSentinelPointer;
}

constexpr auto kMemberDeletedValue = cppgc::kSentinelPointer;

template <typename T>
struct ThreadingTrait<blink::Member<T>> {
  STATIC_ONLY(ThreadingTrait);
  static constexpr ThreadAffinity kAffinity = ThreadingTrait<T>::kAffinity;
};

template <typename T>
struct ThreadingTrait<blink::WeakMember<T>> {
  STATIC_ONLY(ThreadingTrait);
  static constexpr ThreadAffinity kAffinity = ThreadingTrait<T>::kAffinity;
};

template <typename T>
struct ThreadingTrait<blink::UntracedMember<T>> {
  STATIC_ONLY(ThreadingTrait);
  static constexpr ThreadAffinity kAffinity = ThreadingTrait<T>::kAffinity;
};

template <typename T>
inline void swap(Member<T>& a, Member<T>& b) {
  a.Swap(b);
}

static constexpr bool kBlinkMemberGCHasDebugChecks =
    !std::is_same<cppgc::internal::DefaultMemberCheckingPolicy,
                  cppgc::internal::DisabledCheckingPolicy>::value;

// We should never bloat the Member<> wrapper.
// NOTE: The Member<void*> works as we never use this Member in a trace method.
static_assert(kBlinkMemberGCHasDebugChecks ||
                  sizeof(Member<void*>) <= sizeof(void*),
              "Member<> should stay small!");

template <typename T>
struct IsTraceable<Member<T>> {
  STATIC_ONLY(IsTraceable);
  static const bool value = true;
};

template <typename T>
struct IsWeak<WeakMember<T>> : std::true_type {};

template <typename T>
struct IsTraceable<WeakMember<T>> {
  STATIC_ONLY(IsTraceable);
  static const bool value = true;
};

// Peeker type that allows for using all kinds of Member, Persistent, and T*
// interchangeably. This is necessary for collection methods that are called
// directly with any of those types.
template <typename T>
class ValuePeeker final {
  STACK_ALLOCATED();

 public:
  // NOLINTNEXTLINE
  ALWAYS_INLINE ValuePeeker(T* ptr) : ptr_(ptr) {}
  template <typename U>
  // NOLINTNEXTLINE
  ALWAYS_INLINE ValuePeeker(const Member<U>& m) : ptr_(m.Get()) {}
  template <typename U>
  // NOLINTNEXTLINE
  ALWAYS_INLINE ValuePeeker(const WeakMember<U>& m) : ptr_(m.Get()) {}
  template <typename U>
  // NOLINTNEXTLINE
  ALWAYS_INLINE ValuePeeker(const UntracedMember<U>& m) : ptr_(m.Get()) {}
  template <typename U>
  // NOLINTNEXTLINE
  ALWAYS_INLINE ValuePeeker(const Persistent<U>& p) : ptr_(p.Get()) {}
  template <typename U>
  // NOLINTNEXTLINE
  ALWAYS_INLINE ValuePeeker(const WeakPersistent<U>& p) : ptr_(p.Get()) {}

  // NOLINTNEXTLINE
  ALWAYS_INLINE operator T*() const { return ptr_; }
  // NOLINTNEXTLINE
  ALWAYS_INLINE operator Member<T>() const { return ptr_; }
  // NOLINTNEXTLINE
  ALWAYS_INLINE operator WeakMember<T>() const { return ptr_; }
  // NOLINTNEXTLINE
  ALWAYS_INLINE operator UntracedMember<T>() const { return ptr_; }

 private:
  T* ptr_;
};

// Default hash for hash tables with Member<>-derived elements.
template <typename T, typename MemberType>
struct BaseMemberHashTraits : SimpleClassHashTraits<MemberType> {
  STATIC_ONLY(BaseMemberHashTraits);

  // Heap hash containers allow to operate with raw pointers, e.g.
  //   HeapHashSet<Member<GCed>> set;
  //   set.find(raw_ptr);
  // Therefore, provide two hashing functions, one for raw pointers, another for
  // Member. Prefer compressing raw pointers instead of decompressing Members,
  // assuming the former is cheaper.
  static uint32_t GetHash(const T* key) {
    uint32_t record_replay_hash;
    if (RecordReplayIdHash(key, &record_replay_hash)) {
      return record_replay_hash;
    }
#if defined(CPPGC_POINTER_COMPRESSION)
    cppgc::internal::CompressedPointer st(key);
#else
    cppgc::internal::RawPointer st(key);
#endif
    return blink::GetHash(st.GetAsInteger());
  }
  template <typename Member>
    requires(IsAnyMemberType<Member>::value)
  static uint32_t GetHash(const Member& m) {
    if constexpr (kHasRecordReplayId<T>) {
      return GetHash(static_cast<const T*>(m.Get()));
    } else {
      return blink::GetHash(m.GetRawStorage().GetAsInteger());
    }
  }

  // Replay's default for tables of Member<>-derived elements, as in its
  // Chromium 108 (DefaultHashTypeForMember): an element whose type has a
  // RecordReplayId() is hashed by that ID while recording or replaying, so
  // every such table has the same order in a recording and in each replay of
  // it, where an address isn't the same.
  //
  // Whether the type has an ID is asked where a key is hashed, not where the
  // table's type is named, and the type has to be complete there: a file that
  // hashed the same table by address because it never saw the type would
  // disagree with the files that did.
  template <typename U>
  static constexpr bool kHasRecordReplayId = requires(const U* u) {
    static_cast<int>(u->RecordReplayId());
  };
  static bool RecordReplayIdHash(const T* key, uint32_t* hash) {
    static_assert(sizeof(T) > 0,
                  "a table of Member<T> is hashed where T isn't defined: "
                  "include T's header");
    if constexpr (kHasRecordReplayId<T>) {
      if (key && recordreplay::IsRecordingOrReplaying("pointer-ids")) {
        int id = static_cast<int>(key->RecordReplayId());
        // An object made where IDs aren't given has none, and is hashed by
        // its address.
        if (id > 0) {
          *hash = blink::GetHash(static_cast<uint32_t>(id));
          return true;
        }
      }
    }
    return false;
  }

  static constexpr bool kEmptyValueIsZero = true;

  using PeekInType = ValuePeeker<T>;
  using PeekOutType = T*;
  using IteratorGetType = MemberType*;
  using IteratorConstGetType = const MemberType*;
  using IteratorReferenceType = MemberType&;
  using IteratorConstReferenceType = const MemberType&;

  static PeekOutType Peek(const MemberType& value) { return value.Get(); }

  static void ConstructDeletedValue(MemberType& slot) {
    slot = cppgc::kSentinelPointer;
  }

  static bool IsDeletedValue(const MemberType& value) {
    return value == cppgc::kSentinelPointer;
  }
};

// Custom HashTraits<Member<Type>> can inherit this type.
template <typename T>
struct MemberHashTraits : BaseMemberHashTraits<T, blink::Member<T>> {
  static constexpr bool kCanTraceConcurrently = true;
  static constexpr bool kSupportsCompaction = true;
};
template <typename T>
struct HashTraits<blink::Member<T>> : MemberHashTraits<T> {};

// Custom HashTraits<WeakMember<Type>> can inherit this type.
template <typename T>
struct WeakMemberHashTraits : BaseMemberHashTraits<T, blink::WeakMember<T>> {
  static constexpr bool kCanTraceConcurrently = true;
  static constexpr bool kSupportsCompaction = true;
};
template <typename T>
struct HashTraits<blink::WeakMember<T>> : WeakMemberHashTraits<T> {};

// Replay's traits for tables of Member<>-derived elements, as in its Chromium
// 108 (MemberHashRecordReplayRegisteredPointerId): an element is hashed by
// the ID its pointer was registered with, so a table has the same order in a
// recording and every replay of it, where an address isn't the same.
template <typename T, typename MemberType>
struct BaseMemberHashTraitsRecordReplayRegisteredPointerId
    : BaseMemberHashTraits<T, MemberType> {
  static uint32_t GetHash(const T* key) {
    if (recordreplay::IsRecordingOrReplaying("pointer-ids")) {
      if (int id = recordreplay::PointerId(key)) {
        return blink::GetHash(static_cast<uint32_t>(id));
      }
    }
    return BaseMemberHashTraits<T, MemberType>::GetHash(key);
  }
  template <typename Member>
    requires(IsAnyMemberType<Member>::value)
  static uint32_t GetHash(const Member& m) {
    return GetHash(m.Get());
  }

  static constexpr bool kIsRecordReplayDeterministicHash = true;
};
template <typename T>
struct MemberHashTraitsRecordReplayRegisteredPointerId
    : BaseMemberHashTraitsRecordReplayRegisteredPointerId<T, blink::Member<T>> {
  static constexpr bool kCanTraceConcurrently = true;
  static constexpr bool kSupportsCompaction = true;
};
template <typename T>
struct WeakMemberHashTraitsRecordReplayRegisteredPointerId
    : BaseMemberHashTraitsRecordReplayRegisteredPointerId<
          T,
          blink::WeakMember<T>> {
  static constexpr bool kCanTraceConcurrently = true;
  static constexpr bool kSupportsCompaction = true;
};

// The same for elements that have a RecordReplayId() of their own (108's
// MemberHashTraitsRecordReplayId).
template <typename T, typename MemberType>
struct BaseMemberHashTraitsRecordReplayId
    : BaseMemberHashTraits<T, MemberType> {
  static uint32_t GetHash(const T* key) {
    if (recordreplay::IsRecordingOrReplaying("pointer-ids")) {
      int id = key->RecordReplayId();
      // Ids are allowed to be zero if we've diverged from the recording.
      if (recordreplay::HasDivergedFromRecording()) {
        if (id > 0) {
          return blink::GetHash(static_cast<uint32_t>(id));
        }
      } else {
        CHECK(id > 0);  // Should have been registered.
        return blink::GetHash(static_cast<uint32_t>(id));
      }
    }
    return BaseMemberHashTraits<T, MemberType>::GetHash(key);
  }
  template <typename Member>
    requires(IsAnyMemberType<Member>::value)
  static uint32_t GetHash(const Member& m) {
    return GetHash(m.Get());
  }

  static constexpr bool kIsRecordReplayDeterministicHash = true;
};
template <typename T>
struct MemberHashTraitsRecordReplayId
    : BaseMemberHashTraitsRecordReplayId<T, blink::Member<T>> {
  static constexpr bool kCanTraceConcurrently = true;
  static constexpr bool kSupportsCompaction = true;
};
template <typename T>
struct WeakMemberHashTraitsRecordReplayId
    : BaseMemberHashTraitsRecordReplayId<T, blink::WeakMember<T>> {
  static constexpr bool kCanTraceConcurrently = true;
  static constexpr bool kSupportsCompaction = true;
};

template <typename T>
struct UntracedMemberHashTraitsRecordReplayId
    : BaseMemberHashTraitsRecordReplayId<T, blink::UntracedMember<T>> {};

// Custom HashTraits<UntracedMember<Type>> can inherit this type.
template <typename T>
struct UntracedMemberHashTraits
    : BaseMemberHashTraits<T, blink::UntracedMember<T>> {};
template <typename T>
struct HashTraits<blink::UntracedMember<T>> : UntracedMemberHashTraits<T> {};

template <typename T>
class MemberConstructTraits {
  STATIC_ONLY(MemberConstructTraits);

 public:
  template <typename... Args>
  static T* Construct(void* location, Args&&... args) {
    // `Construct()` creates a new Member which must not be visible to the
    // concurrent marker yet, similar to regular ctors in Member.
    return new (base::NotNullTag::kNotNull, location)
        T(std::forward<Args>(args)...);
  }

  template <typename... Args>
  static T* ConstructAndNotifyElement(void* location, Args&&... args) {
    // `ConstructAndNotifyElement()` updates an existing Member which might
    // also be concurrently traced while we update it. The regular ctors
    // for Member don't use an atomic write which can lead to data races.
    T* object = new (base::NotNullTag::kNotNull, location)
        T(std::forward<Args>(args)..., typename T::AtomicInitializerTag());
    NotifyNewElement(object);
    return object;
  }

  static void NotifyNewElement(T* element) {
    WriteBarrier::DispatchForObject(element);
  }

  static void NotifyNewElements(base::span<T> members) {
    // TODO(mlippautz): We can expose whether the write barrier is enabled at
    // all and get away with a single global variable check here.

    // We need a non-null pointer to check if the write barrier is needed.
    // nullptr will just always bail out.
    auto current = members.begin();
    for (; current != members.end(); ++current) {
      if (static_cast<bool>(*current)) {
        break;
      }
    }
    if (current == members.end()) {
      return;
    }
    // Checking one pointer is sufficient for determining whether a
    // marking or generational barrier is required.
    if (!WriteBarrier::IsWriteBarrierNeeded(&*current)) {
      return;
    }
    for (; current != members.end(); ++current) {
      WriteBarrier::DispatchForObject(&*current);
    }
  }
};

template <typename T, typename Traits, typename Allocator>
class ConstructTraits<Member<T>, Traits, Allocator> final
    : public MemberConstructTraits<Member<T>> {};

template <typename T, typename Traits, typename Allocator>
class ConstructTraits<WeakMember<T>, Traits, Allocator> final
    : public MemberConstructTraits<WeakMember<T>> {};

template <typename T>
struct IsPointerType<blink::WeakMember<T>> : std::true_type {};

template <typename T>
struct IsPointerType<blink::Member<T>> : std::true_type {};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_HEAP_MEMBER_H_
