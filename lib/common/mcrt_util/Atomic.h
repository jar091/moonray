// Copyright 2023-2025 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0
#pragma once

//
// The following directive is enabled, when we don't have a 128 bit (=16bytes) "lock-free" atomic operation 
// Usually, this directive is properly set by cmake. See moonray/cmake/MoonrayCheckFeature.cmake
//
//#define NO_16BYTE_ATOMIC_LOCK_FREE

#include <scene_rdl2/common/platform/Platform.h>
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
#include <scene_rdl2/render/util/Atomic128.h>
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE

#include <atomic>
#include <cstdint>
#include <memory>

#if defined(_MSC_VER)
// MSVC has no __atomic_* builtins. The generic operations below only need
// load / store / compare-exchange on trivially-copyable objects of 4, 8 or
// 16 bytes. 4 and 8 bytes map onto std::atomic over an equally sized integral
// type; 16 bytes go straight to _InterlockedCompareExchange128 (see below).
// Everything goes through memcpy, so no strict-aliasing or type-punning
// assumptions are made.
#include <cstring>
#include <intrin.h> // _InterlockedCompareExchange128
// Alignment hint only; MSVC has no equivalent, and the callers assert the
// alignment they rely on.
#define __builtin_assume_aligned(ptr, alignment) (ptr)
#endif

namespace moonray {
namespace util {

// TODO: This is duplicated from scene_rdl2 AtomicFloat
namespace atomic_detail {

#if !defined(_MSC_VER)
// We do a lot of casting to integer values for the intrinsics interface. Let's make sure we are casting the values we
// think we are.
static_assert(__ATOMIC_RELAXED == static_cast<int>(std::memory_order_relaxed));
static_assert(__ATOMIC_CONSUME == static_cast<int>(std::memory_order_consume));
static_assert(__ATOMIC_ACQUIRE == static_cast<int>(std::memory_order_acquire));
static_assert(__ATOMIC_RELEASE == static_cast<int>(std::memory_order_release));
static_assert(__ATOMIC_ACQ_REL == static_cast<int>(std::memory_order_acq_rel));
static_assert(__ATOMIC_SEQ_CST == static_cast<int>(std::memory_order_seq_cst));
#endif

#if defined(_MSC_VER)

//
// 16-byte (double-quadword) primitives.
//
// MSVC's std::atomic has *no* lock-free 16-byte specialization: sizeof(T) == 16
// selects the primary _Atomic_storage template, which stores a spinlock in
// front of the value (only std::atomic_ref gets the cmpxchg16b specialization).
// So std::atomic cannot be used here at all - it is neither lock-free nor
// layout-compatible with the caller's object.
//
// The x64 hardware primitive is _InterlockedCompareExchange128, which is
// exactly the `lock cmpxchg16b` that the GCC path issues by hand in
// scene_rdl2's Atomic128.h. Like that path it is always sequentially
// consistent - the lock prefix makes the instruction a full barrier and there
// is no weaker form and no weak-CAS form - so a requested memory order can
// only ever be strengthened here, never weakened.
//
// The 16 bytes at `dest` must be 16-byte aligned or the instruction faults;
// that is the same requirement the GCC path has, and the callers assert it.
//
constexpr std::size_t kDcasBytes = 16;

// Returns true if the exchange happened. Otherwise `expected` receives the
// current contents of `dest`, matching __atomic_compare_exchange().
inline bool
dcas(volatile void* dest, void* expected, const void* desired) noexcept
{
    // _InterlockedCompareExchange128 takes the exchange value split into
    // (high, low) and a 2-element comparand array which it *updates in place*
    // with the current contents of *dest whenever the exchange does not
    // happen. Copying through locals keeps the caller's type out of the
    // __int64 aliasing and guarantees the comparand's alignment.
    alignas(kDcasBytes) __int64 expectedWords[2];
    alignas(kDcasBytes) __int64 desiredWords[2];
    std::memcpy(expectedWords, expected, kDcasBytes);
    std::memcpy(desiredWords, desired, kDcasBytes);

    const unsigned char exchanged =
        _InterlockedCompareExchange128(reinterpret_cast<volatile __int64*>(dest),
                                       desiredWords[1], // high 64 bits (RCX)
                                       desiredWords[0], // low 64 bits (RBX)
                                       expectedWords);  // RDX:RAX, updated on failure
    if (!exchanged) {
        std::memcpy(expected, expectedWords, kDcasBytes);
    }
    return exchanged != 0;
}

inline void
dload(const volatile void* src, void* dest) noexcept
{
    // Comparing 0 against 0 leaves *src untouched unless it already is 0 (in
    // which case 0 is written back), and reports the current contents through
    // the comparand either way. This is how the MSVC STL itself implements a
    // 16-byte load. Note it takes write ownership of the cache line, exactly
    // like the CAS loop in scene_rdl2's atomicLoad128().
    alignas(kDcasBytes) __int64 observed[2] = {0, 0};
    _InterlockedCompareExchange128(
        reinterpret_cast<volatile __int64*>(const_cast<volatile void*>(src)), 0, 0, observed);
    std::memcpy(dest, observed, kDcasBytes);
}

inline void
dstore(volatile void* dest, const void* val) noexcept
{
    alignas(kDcasBytes) __int64 desiredWords[2];
    std::memcpy(desiredWords, val, kDcasBytes);

    // The comparand is refreshed with the current value on every failure, so
    // the loop always re-tries against what is really there. Same CAS-loop
    // store (and same ABA/live-lock caveats) as scene_rdl2's atomicStore128().
    alignas(kDcasBytes) __int64 observed[2] = {0, 0};
    while (!_InterlockedCompareExchange128(reinterpret_cast<volatile __int64*>(dest),
                                           desiredWords[1], desiredWords[0], observed)) {
    }
}

// Storage type of the same size as T that std::atomic handles natively.
// 16-byte objects are not listed: they go through dcas/dload/dstore above.
template <typename T>
struct AtomicStorage;

template <typename T> struct AtomicStorageOfSize;
template <> struct AtomicStorageOfSize<std::integral_constant<std::size_t, 1>>  { using type = std::uint8_t; };
template <> struct AtomicStorageOfSize<std::integral_constant<std::size_t, 2>>  { using type = std::uint16_t; };
template <> struct AtomicStorageOfSize<std::integral_constant<std::size_t, 4>>  { using type = std::uint32_t; };
template <> struct AtomicStorageOfSize<std::integral_constant<std::size_t, 8>>  { using type = std::uint64_t; };

template <typename T>
using AtomicStorageT =
    typename AtomicStorageOfSize<std::integral_constant<std::size_t, sizeof(T)>>::type;

// Reinterpret the caller's object as its atomic storage type. Both types are
// trivially copyable and the same size; std::atomic<storage> has the same
// size and alignment requirements, and MSVC's implementation operates
// in-place on the referenced object.
template <typename T>
inline std::atomic<AtomicStorageT<T>>* asAtomic(T* p) noexcept
{
    static_assert(std::is_trivially_copyable<T>::value, "atomic ops need trivially copyable types");
    return reinterpret_cast<std::atomic<AtomicStorageT<T>>*>(p);
}

template <typename T>
inline const std::atomic<AtomicStorageT<T>>* asAtomic(const T* p) noexcept
{
    static_assert(std::is_trivially_copyable<T>::value, "atomic ops need trivially copyable types");
    return reinterpret_cast<const std::atomic<AtomicStorageT<T>>*>(p);
}

template <typename T>
inline AtomicStorageT<T> toStorage(const T& v) noexcept
{
    AtomicStorageT<T> s;
    std::memcpy(&s, std::addressof(v), sizeof(T));
    return s;
}

template <typename T>
inline T fromStorage(const AtomicStorageT<T>& s) noexcept
{
    T v;
    std::memcpy(std::addressof(v), &s, sizeof(T));
    return v;
}

#endif // _MSC_VER

// In compare_exchange overloads where only one memory order is given, we have
// to decide on the other. These are what is laid out by the standard.
constexpr std::memory_order compare_exchange_duo(std::memory_order in) noexcept
{
    constexpr std::memory_order mapping[6] = {
            /* std::memory_order_relaxed -> */ std::memory_order_relaxed,
            /* std::memory_order_consume -> */ std::memory_order_consume,
            /* std::memory_order_acquire -> */ std::memory_order_acquire,
            /* std::memory_order_release -> */ std::memory_order_relaxed,
            /* std::memory_order_acq_rel -> */ std::memory_order_acquire,
            /* std::memory_order_seq_cst -> */ std::memory_order_seq_cst

    };
    return mapping[static_cast<int>(in)];
}
} // namespace atomic_detail

// A double-quad-word is 16 bytes == 128 bits.
// Aligning on the size of the data type guarantees that we are not splitting a cache-line. This does, however, mean
// that there may be false sharing. If you want to avoid false sharing, you should explicitly align on cache line
// size.
//
// Cache-line size is defined as CACHE_LINE_SIZE or std::hardware_destructive_interference_size (C++17).
constexpr std::size_t kDoubleQuadWordAtomicAlignment = 16u;

template <typename T>
constexpr std::size_t atomicAlignment() noexcept
{
    // Aligning on the size of the data type guarantees that we are not splitting a cache-line. This does, however, mean
    // that there may be false sharing. If you want to avoid false sharing, you should explicitly align on cache line
    // size.
    //
    // Cache-line size is defined as CACHE_LINE_SIZE or std::hardware_destructive_interference_size (C++17).
    return sizeof(T);
}

// Up to and including ICC 19/GCC 8.5, the library doesn't define the nested value_type for std:atomic (boo!). Write our
// own value_type helper.
template <typename T>
struct atomic_value_type
{
    using value_type = T;
};

template <typename T>
struct atomic_value_type<std::atomic<T>>
{
    using value_type = T;
};

template <typename T>
inline T
atomicLoad(const T* v, std::memory_order order = std::memory_order_seq_cst) noexcept
{
#if defined(_MSC_VER)
    if constexpr (sizeof(T) == 16) { // 128bit: always sequentially consistent
        alignas(atomicAlignment<T>()) unsigned char buf[sizeof(T)];
        atomic_detail::dload(v, buf);
        return *reinterpret_cast<const T*>(buf);
    } else {
        return atomic_detail::fromStorage<T>(atomic_detail::asAtomic(v)->load(order));
    }
#else
    alignas(T) unsigned char buf[sizeof(T)];
    auto* const dest = reinterpret_cast<T*>(buf);
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    if constexpr (sizeof(T) == 16) { // 128bit
        // Always executed under __ATOMIC_SEQ_CST:Sequencial Consistency memory order
        scene_rdl2::util::atomicLoad128(const_cast<volatile void*>(reinterpret_cast<const volatile void*>(v)),
                                        dest);
    } else {
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
        MNRY_ASSERT(__atomic_is_lock_free(sizeof(T), v));
        __atomic_load(v, dest, static_cast<int>(order));
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    }
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
    return *dest;
#endif // _MSC_VER
}

template <typename T>
void
atomicStore(T* v, T val, std::memory_order order = std::memory_order_seq_cst) noexcept
{
#if defined(_MSC_VER)
    if constexpr (sizeof(T) == 16) { // 128bit: always sequentially consistent
        atomic_detail::dstore(v, std::addressof(val));
    } else {
        atomic_detail::asAtomic(v)->store(atomic_detail::toStorage(val), order);
    }
#else
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    if constexpr (sizeof(T) == 16) { // 128bit
        // Always executed under __ATOMIC_SEQ_CST:Sequencial Consistency memory order
        scene_rdl2::util::atomicStore128(v, std::addressof(val));
    } else {
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
        MNRY_ASSERT(__atomic_is_lock_free(sizeof(T), v));
        __atomic_store(v, std::addressof(val), static_cast<int>(order));
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    }
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
#endif // _MSC_VER
}

template <typename T>
inline bool
atomicCompareAndSwapWeak(T* v, T& expected, T desired, std::memory_order success, std::memory_order failure) noexcept
{
#if defined(_MSC_VER)
    if constexpr (sizeof(T) == 16) { // 128bit
        // Always sequentially consistent, and always a strong CAS: cmpxchg16b
        // has no weak form (same as the GCC/Atomic128.h path).
        return atomic_detail::dcas(v, std::addressof(expected), std::addressof(desired));
    } else {
        auto expectedStorage = atomic_detail::toStorage(expected);
        const bool ok = atomic_detail::asAtomic(v)->compare_exchange_weak(
            expectedStorage, atomic_detail::toStorage(desired), success, failure);
        if (!ok) {
            expected = atomic_detail::fromStorage<T>(expectedStorage);
        }
        return ok;
    }
#else
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    if constexpr (sizeof(T) == 16) { // 128bit
        // Always executed under __ATOMIC_SEQ_CST:Sequencial Consistency memory order
        // Also there is no option for Weak CAS operation. Always executes as Strong CAS.
        return scene_rdl2::util::atomicCmpxchg128(v,
                                                  std::addressof(expected),
                                                  std::addressof(desired));
    } else {
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
        MNRY_ASSERT(__atomic_is_lock_free(sizeof(T), v));
        return __atomic_compare_exchange(v,
                                         std::addressof(expected),
                                         std::addressof(desired),
                                         true,
                                         static_cast<int>(success),
                                         static_cast<int>(failure));
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    }
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
#endif // _MSC_VER
}

template <typename T>
inline bool
atomicCompareAndSwapWeak(T* v, T& expected, T desired, std::memory_order order = std::memory_order_seq_cst) noexcept
{
#if !defined(_MSC_VER)
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    if constexpr (sizeof(T) != 16) {
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
        MNRY_ASSERT(__atomic_is_lock_free(sizeof(T), v));
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    }
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
#endif // !_MSC_VER
    return atomicCompareAndSwapWeak(v, expected, desired, order, atomic_detail::compare_exchange_duo(order));
}

template <typename T>
inline bool
atomicCompareAndSwapStrong(T* v, T& expected, T desired, std::memory_order success, std::memory_order failure) noexcept
{
#if defined(_MSC_VER)
    if constexpr (sizeof(T) == 16) { // 128bit: always sequentially consistent
        return atomic_detail::dcas(v, std::addressof(expected), std::addressof(desired));
    } else {
        auto expectedStorage = atomic_detail::toStorage(expected);
        const bool ok = atomic_detail::asAtomic(v)->compare_exchange_strong(
            expectedStorage, atomic_detail::toStorage(desired), success, failure);
        if (!ok) {
            expected = atomic_detail::fromStorage<T>(expectedStorage);
        }
        return ok;
    }
#else
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    if constexpr (sizeof(T) == 16) { // 128bit
        // Always executed under __ATOMIC_SEQ_CST:Sequencial Consistency memory order
        return scene_rdl2::util::atomicCmpxchg128(v,
                                                  std::addressof(expected),
                                                  std::addressof(desired));
    } else {
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
        MNRY_ASSERT(__atomic_is_lock_free(sizeof(T), v));
        return __atomic_compare_exchange(v,
                                         std::addressof(expected),
                                         std::addressof(desired),
                                         false,
                                         static_cast<int>(success),
                                         static_cast<int>(failure));
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    }
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
#endif // _MSC_VER
}

template <typename T>
inline bool
atomicCompareAndSwapStrong(T* v, T& expected, T desired, std::memory_order order = std::memory_order_seq_cst) noexcept
{
#if !defined(_MSC_VER)
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    if constexpr (sizeof(T) != 16) {
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
        MNRY_ASSERT(__atomic_is_lock_free(sizeof(T), v));
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    }
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
#endif // !_MSC_VER
    return atomicCompareAndSwapStrong(v, expected, desired, order, atomic_detail::compare_exchange_duo(order));
}

template <typename T>
inline void
atomicAssignFloat(T* val, T newValue, std::memory_order order = std::memory_order_seq_cst) noexcept
{
#if defined(_MSC_VER)
    atomicStore(val, newValue, order);
#else
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    if constexpr (sizeof(T) == 16) { // 128bit
        // Always executed under __ATOMIC_SEQ_CST:Sequencial Consistency memory order
        scene_rdl2::util::atomicStore128(val, std::addressof(newValue));
    } else {
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
        MNRY_ASSERT(__atomic_is_lock_free(sizeof(T), val));
        __atomic_store(val, std::addressof(newValue), static_cast<int>(order));
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    }
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
#endif // _MSC_VER
}

// We want to store the min of the two values (_a_, _b_) in _a_
template <typename T>
inline void
atomicMin(T* a, T b) noexcept
{
    alignas(atomicAlignment<T>()) T x = atomicLoad(a, std::memory_order_relaxed);

#if !defined(_MSC_VER)
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    if constexpr (sizeof(T) != 16) {
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
        MNRY_ASSERT(__atomic_is_lock_free(sizeof(T), a));
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    }
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
#endif // !_MSC_VER

    do {
        // If _x_ is less than or equal to _b_, our work is done. If some other thread calls in, the value of _x_ is
        // only going to get lower, and b is still greater.
        if (x <= b) {
            break;
        }
        // If we get to the CAS, we know that _b_ is less than _a_ (unless another thread preempted us). Update _a_ to
        // the value of _b_. If it succeeds, we're done! If it fails, _x_ is updated to the new value of _a_, and we
        // will continue and check if the new value is less than _b_.
    } while (!atomicCompareAndSwapWeak(a, x, b, std::memory_order_relaxed));
}

// We want to store the max of the two values (_a_, _b_) in _a_
template <typename T>
inline void
atomicMax(T* a, T b) noexcept
{
    alignas(atomicAlignment<T>()) T x = atomicLoad(a, std::memory_order_relaxed);

#if !defined(_MSC_VER)
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    if constexpr (sizeof(T) != 16) {
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
        MNRY_ASSERT(__atomic_is_lock_free(sizeof(T), a));
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    }
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
#endif // !_MSC_VER

    do {
        // If _x_ is greater than or equal to _b_, our work is done. If some other thread calls in, the value of x is
        // only going to get higher, and b is still smaller.
        if (b <= x) {
            break;
        }
        // If we get to the CAS, we know that _b_ is greater than _a_ (unless another thread preempted us). Update _a_
        // to the value of _b_. If it succeeds, we're done! If it fails, _x_ is updated to the new value of _a_, and we
        // will continue and check if the new value is greater than _b_.
    } while (!atomicCompareAndSwapWeak(a, x, b, std::memory_order_relaxed));
}

// We want to store the sum of the two values (_a_, _b_) in _a_
template <typename T>
inline void
atomicAdd(T* a, T b) noexcept
{
    alignas(atomicAlignment<T>()) T oldVal = atomicLoad(a, std::memory_order_relaxed);
    alignas(atomicAlignment<T>()) T newVal = oldVal + b;

#if !defined(_MSC_VER)
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    if constexpr (sizeof(T) != 16) {
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
        MNRY_ASSERT(__atomic_is_lock_free(sizeof(T), a));
#ifdef NO_16BYTE_ATOMIC_LOCK_FREE
    }
#endif // end of NO_16BYTE_ATOMIC_LOCK_FREE
#endif // !_MSC_VER

    while (!atomicCompareAndSwapWeak(a, oldVal, newVal, std::memory_order_relaxed)) {
        newVal = oldVal + b;
    }
}

inline void
atomicLoadFloat4(float* __restrict dst, const float* __restrict src)
{
    // We don't allow our array of four floats to overlap since we're using restrict.
    MNRY_ASSERT(dst + 4 <= src || src + 4 <= dst);

    MNRY_ASSERT(reinterpret_cast<uintptr_t>(dst) % kDoubleQuadWordAtomicAlignment == 0);
    MNRY_ASSERT(reinterpret_cast<uintptr_t>(src) % kDoubleQuadWordAtomicAlignment == 0);

    // assumes 1) dst and src point to aligned float[4] types
    //         2) These types should produce lock-free atomics

    // We don't define this struct as aligned, because that changes the size.
    // So, for instance, if somebody decides that we should align all
    // double-quad words on 64-bytes (to avoid false sharing), that will make
    // the structure 64-bytes, and probably no longer atomic.
    struct Float4Aligned
    {
        float x;
        float y;
        float z;
        float w;
    };

    static_assert(sizeof(Float4Aligned) == 16,
                  "If it's bigger, our architecture (at the time of this writing) won't make it atomic");

    const auto dstStruct = static_cast<Float4Aligned*>(
        __builtin_assume_aligned(reinterpret_cast<void*>(dst), kDoubleQuadWordAtomicAlignment));
    const auto srcStruct = static_cast<const Float4Aligned*>(
        __builtin_assume_aligned(reinterpret_cast<const void*>(src), kDoubleQuadWordAtomicAlignment));

    MNRY_ASSERT(reinterpret_cast<uintptr_t>(dstStruct) % kDoubleQuadWordAtomicAlignment == 0);
    MNRY_ASSERT(reinterpret_cast<uintptr_t>(srcStruct) % kDoubleQuadWordAtomicAlignment == 0);

    // This may not be true, because it's architecture dependent, which the compiler doesn't know.
    // static_assert(__atomic_always_lock_free(sizeof(Float4Aligned), nullptr), "");

#if defined(_MSC_VER)
    *dstStruct = atomicLoad(srcStruct, std::memory_order_relaxed);
#elif defined(NO_16BYTE_ATOMIC_LOCK_FREE)
    // Always executed under __ATOMIC_SEQ_CST:Sequencial Consistency memory order
    scene_rdl2::util::atomicLoad128(const_cast<volatile void*>(reinterpret_cast<const volatile void*>(srcStruct)),
                                    dstStruct);
#else // else of NO_16BYTE_ATOMIC_LOCK_FREE
    MNRY_ASSERT(__atomic_is_lock_free(sizeof(Float4Aligned), srcStruct));
    __atomic_load(srcStruct, dstStruct, __ATOMIC_RELAXED);
#endif // end of Not NO_16BYTE_ATOMIC_LOCK_FREE
}

inline void
atomicAssignIfClosest(float* __restrict val, const float* __restrict newVal)
{
    // We don't allow our array of four floats to overlap since we're using restrict.
    MNRY_ASSERT(val + 4 <= newVal || newVal + 4 <= val);

    MNRY_ASSERT(reinterpret_cast<uintptr_t>(val) % kDoubleQuadWordAtomicAlignment == 0);
    MNRY_ASSERT(reinterpret_cast<uintptr_t>(newVal) % kDoubleQuadWordAtomicAlignment == 0);

    // assumes 1) val and newVal point to aligned float[4] types
    //         2) val[3] and newVal[3] store the depth
    //         3) These types should produce lock-free atomics

    // We don't define this struct as aligned, because that changes the size.
    // So, for instance, if somebody decides that we should align all
    // double-quad words on 64-bytes (to avoid false sharing), that will make
    // the structure 64-bytes, and probably no longer atomic.
    struct Float4Aligned
    {
        float x, y, z, d;
    };

    static_assert(sizeof(Float4Aligned) == 16,
                  "If it's bigger, our architecture (at the time of this writing) won't make it atomic");

    const auto dstStruct =
        static_cast<Float4Aligned*>(__builtin_assume_aligned(reinterpret_cast<void*>(val),
                                                             kDoubleQuadWordAtomicAlignment));
    const auto srcStruct =
        static_cast<const Float4Aligned*>(__builtin_assume_aligned(reinterpret_cast<const void*>(newVal),
                                                                   kDoubleQuadWordAtomicAlignment));

    Float4Aligned observed;
    observed = atomicLoad(dstStruct, std::memory_order_relaxed);
    do {
        // If _observed.d_ is less than or equal to _srcVal.d_, our work is done. If some other thread calls in, the
        // value of _dest.d_ is only going to get lower, so _srcVal.d_ is still greater.
        if (observed.d <= srcStruct->d) {
            break;
        }
        // If we get to the CAS, we know that _srcVal.d_ is less than _dest->d_ (unless another thread preempted us).
        // Update _dest_ to the value of _srcVal_. If it succeeds, we're done! If it fails, _observed_ is updated to the
        // new value of _dest_ and we will continue and check if the new value is less than _srcVal_.
    } while (!atomicCompareAndSwapWeak(dstStruct, observed, *srcStruct, std::memory_order_relaxed));
}

} // namespace util
} // namespace moonray
