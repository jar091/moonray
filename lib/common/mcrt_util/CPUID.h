// Copyright 2023-2024 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

#pragma once

#if !defined(__APPLE__)

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <tuple>
#include <type_traits>

#if defined(_WIN32)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

#pragma warning push
#pragma warning(disable:1711) // Assignment to static variable

// DLL-interface macro: CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS exports functions but
// not data, and CPUID's static data members are filled in by CPUID.cc yet read
// through this header's inline accessors from other libraries.
// common_mcrt_util_EXPORTS is defined by CMake while building the DLL itself.
#if defined(_WIN32)
  #if defined(common_mcrt_util_EXPORTS)
    #define MOONRAY_MCRT_UTIL_EXPORT __declspec(dllexport)
  #else
    #define MOONRAY_MCRT_UTIL_EXPORT __declspec(dllimport)
  #endif
#else
  #define MOONRAY_MCRT_UTIL_EXPORT
#endif

namespace moonray {
namespace util {

namespace cpuid_detail
{
enum class CPUFeatures : std::uint64_t
{
    NONE                   = 0,
    CPU_FEATURE_SSE        = 0b0000'0000'0000'0000'0000'0001,
    CPU_FEATURE_SSE2       = 0b0000'0000'0000'0000'0000'0010,
    CPU_FEATURE_SSE3       = 0b0000'0000'0000'0000'0000'0100,
    CPU_FEATURE_SSSE3      = 0b0000'0000'0000'0000'0000'1000,
    CPU_FEATURE_SSE41      = 0b0000'0000'0000'0000'0001'0000,
    CPU_FEATURE_SSE42      = 0b0000'0000'0000'0000'0010'0000,
    CPU_FEATURE_POPCNT     = 0b0000'0000'0000'0000'0100'0000,

    CPU_FEATURE_AVX        = 0b0000'0000'0000'0000'1000'0000,
    CPU_FEATURE_F16C       = 0b0000'0000'0000'0001'0000'0000,
    CPU_FEATURE_RDRAND     = 0b0000'0000'0000'0010'0000'0000,
    CPU_FEATURE_AVX2       = 0b0000'0000'0000'0100'0000'0000,
    CPU_FEATURE_FMA4       = 0b0000'0000'0000'1000'0000'0000,
    CPU_FEATURE_LZCNT      = 0b0000'0000'0001'0000'0000'0000,
    CPU_FEATURE_BMI1       = 0b0000'0000'0010'0000'0000'0000,
    CPU_FEATURE_BMI2       = 0b0000'0000'0100'0000'0000'0000,

    CPU_FEATURE_AVX512F    = 0b0000'0000'1000'0000'0000'0000,
    CPU_FEATURE_AVX512DQ   = 0b0000'0001'0000'0000'0000'0000,
    CPU_FEATURE_AVX512PF   = 0b0000'0010'0000'0000'0000'0000,
    CPU_FEATURE_AVX512ER   = 0b0000'0100'0000'0000'0000'0000,
    CPU_FEATURE_AVX512CD   = 0b0000'1000'0000'0000'0000'0000,
    CPU_FEATURE_AVX512BW   = 0b0001'0000'0000'0000'0000'0000,
    CPU_FEATURE_AVX512IFMA = 0b0010'0000'0000'0000'0000'0000,
    CPU_FEATURE_AVX512VL   = 0b0100'0000'0000'0000'0000'0000,
    CPU_FEATURE_AVX512VBMI = 0b1000'0000'0000'0000'0000'0000
};

CPUFeatures operator|(CPUFeatures a, CPUFeatures b) noexcept
{
    using underlying_type = typename std::underlying_type<CPUFeatures>::type;
    return static_cast<CPUFeatures>(static_cast<underlying_type>(a) | static_cast<underlying_type>(b));
}

CPUFeatures& operator|=(CPUFeatures& a, CPUFeatures b) noexcept
{
    return a = a | b;
}

CPUFeatures operator&(CPUFeatures a, CPUFeatures b) noexcept
{
    using underlying_type = typename std::underlying_type<CPUFeatures>::type;
    return static_cast<CPUFeatures>(static_cast<underlying_type>(a) & static_cast<underlying_type>(b));
}

CPUFeatures& operator&=(CPUFeatures& a, CPUFeatures b) noexcept
{
    return a = a & b;
}

bool is_set(CPUFeatures collection, CPUFeatures individual) noexcept
{
    return (collection & individual) == individual;
}

} // namespace cpuid_detail

#if defined(_WIN32)
// MSVC ships no <cpuid.h>. Reimplement the two GCC helpers on top of the
// __cpuid/__cpuidex intrinsics from <intrin.h> with identical semantics:
//   __get_cpuid_max(ext, sig) executes leaf _ext_ (0 for the basic leaves,
//     0x80000000 for the extended ones), stores EBX in *sig when sig is not
//     null, and returns EAX -- the highest supported leaf of that range.
//   __get_cpuid[_count]() return 0 and leave the output registers untouched
//     when the requested leaf is above that maximum, 1 otherwise.
// Getting the maximum-leaf test wrong would silently mis-detect CPU features
// (CPUID returns the highest supported leaf's data for out-of-range leaves on
// Intel, and zeros on AMD), so the check is kept exactly as GCC has it.
// CPUID itself always exists on x86-64, so the 32-bit EFLAGS probe GCC does is
// not needed.
inline unsigned int
__get_cpuid_max(unsigned int ext, unsigned int* sig) noexcept
{
    int regs[4];
    __cpuid(regs, static_cast<int>(ext));
    if (sig) {
        *sig = static_cast<unsigned int>(regs[1]);
    }
    return static_cast<unsigned int>(regs[0]);
}

inline int
__get_cpuid_count(unsigned int leaf, unsigned int subleaf,
                  unsigned int* eax, unsigned int* ebx,
                  unsigned int* ecx, unsigned int* edx) noexcept
{
    const unsigned int maxLeaf = __get_cpuid_max(leaf & 0x80000000U, nullptr);
    if (maxLeaf == 0U || maxLeaf < leaf) {
        return 0;
    }
    int regs[4];
    __cpuidex(regs, static_cast<int>(leaf), static_cast<int>(subleaf));
    *eax = static_cast<unsigned int>(regs[0]);
    *ebx = static_cast<unsigned int>(regs[1]);
    *ecx = static_cast<unsigned int>(regs[2]);
    *edx = static_cast<unsigned int>(regs[3]);
    return 1;
}

inline int
__get_cpuid(unsigned int leaf,
            unsigned int* eax, unsigned int* ebx,
            unsigned int* ecx, unsigned int* edx) noexcept
{
    // MSVC's __cpuid(regs, leaf) is __cpuidex(regs, leaf, 0); the subleaf is
    // ignored by every leaf this header queries through __get_cpuid().
    return __get_cpuid_count(leaf, 0U, eax, ebx, ecx, edx);
}

// The feature bit masks below are the same values <cpuid.h> defines.
#define bit_SSE        0x02000000   // leaf 1, EDX
#define bit_SSE2       0x04000000   // leaf 1, EDX
#define bit_SSE3       0x00000001   // leaf 1, ECX
#define bit_SSSE3      0x00000200   // leaf 1, ECX
#define bit_SSE4_1     0x00080000   // leaf 1, ECX
#define bit_SSE4_2     0x00100000   // leaf 1, ECX
#define bit_POPCNT     0x00800000   // leaf 1, ECX
#define bit_AVX        0x10000000   // leaf 1, ECX
#define bit_F16C       0x20000000   // leaf 1, ECX
#define bit_RDRND      0x40000000   // leaf 1, ECX
#define bit_LZCNT      0x00000020   // leaf 0x80000001, ECX (== bit_ABM)
#define bit_FMA4       0x00010000   // leaf 0x80000001, ECX
#define bit_BMI        0x00000008   // leaf 7/0, EBX
#define bit_AVX2       0x00000020   // leaf 7/0, EBX
#define bit_BMI2       0x00000100   // leaf 7/0, EBX
#define bit_AVX512F    0x00010000   // leaf 7/0, EBX
#define bit_AVX512DQ   0x00020000   // leaf 7/0, EBX
#define bit_AVX512IFMA 0x00200000   // leaf 7/0, EBX
#define bit_AVX512PF   0x04000000   // leaf 7/0, EBX
#define bit_AVX512ER   0x08000000   // leaf 7/0, EBX
#define bit_AVX512CD   0x10000000   // leaf 7/0, EBX
#define bit_AVX512BW   0x40000000   // leaf 7/0, EBX
#define bit_AVX512VL   0x80000000   // leaf 7/0, EBX
#define bit_AVX512VBMI 0x00000002   // leaf 7/0, ECX
#endif // _WIN32

namespace cpuid_detail
{
#if defined(_WIN32)
// MSVC has no 128-bit integer type; a 16-byte trivially copyable struct picks
// the std::atomic specialization of the same width. Note that it is NOT
// lock-free there - see check_atomic() below.
struct UInt128Storage { std::uint64_t lo; std::uint64_t hi; };
using uint128_type = UInt128Storage;
#else
using uint128_type = __uint128_t;
#endif
} // namespace cpuid_detail

class CPUID
{
public:
    CPUID()
    {
        static std::once_flag flag;
        std::call_once(flag, &CPUID::init);
    }

    const std::string& vendor() const noexcept
    {
        return s_vendor;
    }

    bool atomic_8() const noexcept
    {
        return s_atomic_sizes.a8;
    }

    bool atomic_16() const noexcept
    {
        return s_atomic_sizes.a16;
    }

    bool atomic_32() const noexcept
    {
        return s_atomic_sizes.a32;
    }

    bool atomic_64() const noexcept
    {
        return s_atomic_sizes.a64;
    }

    bool atomic_128() const noexcept
    {
        return s_atomic_sizes.a128;
    }

    bool sse() const noexcept
    {
        return cpuid_detail::is_set(s_features, cpuid_detail::CPUFeatures::CPU_FEATURE_SSE);
    }

    bool sse2() const noexcept
    {
        return cpuid_detail::is_set(s_features, cpuid_detail::CPUFeatures::CPU_FEATURE_SSE2);
    }

    bool sse3() const noexcept
    {
        return cpuid_detail::is_set(s_features, cpuid_detail::CPUFeatures::CPU_FEATURE_SSE3);
    }

    bool sse41() const noexcept
    {
        return cpuid_detail::is_set(s_features, cpuid_detail::CPUFeatures::CPU_FEATURE_SSE41);
    }

    bool sse42() const noexcept
    {
        return cpuid_detail::is_set(s_features, cpuid_detail::CPUFeatures::CPU_FEATURE_SSE42);
    }

    bool avx() const noexcept
    {
        return cpuid_detail::is_set(s_features, cpuid_detail::CPUFeatures::CPU_FEATURE_AVX);
    }

    bool avx2() const noexcept
    {
        return cpuid_detail::is_set(s_features, cpuid_detail::CPUFeatures::CPU_FEATURE_AVX2);
    }

    bool avx512() const noexcept
    {
        return cpuid_detail::is_set(s_features, cpuid_detail::CPUFeatures::CPU_FEATURE_AVX512F);
    }

private:
    static constexpr std::size_t log2(std::size_t n) noexcept
    {
        std::size_t r{0};
        while (n >>= 1ULL) {
            ++r;
        }
        return r;
    }

#if defined(_WIN32)
    // CMPXCHG16B: CPUID leaf 1, ECX bit 13.
    static bool has_cmpxchg16b()
    {
        std::array<unsigned, 4> regs = { 0U, 0U, 0U, 0U };
        __get_cpuid(0x00000001U, &regs[0], &regs[1], &regs[2], &regs[3]);
        return (regs[2 /*ECX*/] & (1U << 13)) != 0U;
    }
#endif

    template<std::size_t size>
    static bool check_atomic()
    {
        constexpr std::size_t index = log2(size) - 3ULL;// We start at 8 == 2^3
        using Tuple = std::tuple<std::uint8_t, std::uint16_t, std::uint32_t, std::uint64_t,
                                 cpuid_detail::uint128_type>;
        using Type = std::tuple_element_t<index, Tuple>;
#if defined(_WIN32)
        // MSVC's std::atomic<16-byte object> is NOT lock-free: the
        // specialization is 24 bytes with a spinlock in front of the value, so
        // is_lock_free() is false on every x64 CPU and this row reported
        // "Atomic 128 bit support = 0" even where cmpxchg16b exists. The row
        // sits in the *hardware* table, and what actually performs 16-byte
        // atomics here is Atomic.h's _InterlockedCompareExchange128 branch
        // (which emits `lock cmpxchg16b` and does not use std::atomic at all),
        // so ask the CPU rather than the standard library.
        if constexpr (size == 128) {
            return has_cmpxchg16b();
        }
#endif
        return std::atomic<Type>{}.is_lock_free();
    }

    static std::string get_vendor()
    {
        std::array<unsigned, 4> cpuinfo;
        __get_cpuid(0, &cpuinfo[0], &cpuinfo[1], &cpuinfo[2], &cpuinfo[3]);
        const std::array<unsigned, 4> name = {cpuinfo[1], cpuinfo[3], cpuinfo[2], 0};
        return reinterpret_cast<const char *>(name.data());
    }

    static cpuid_detail::CPUFeatures get_cpu_features()
    {
        using cpuid_detail::CPUFeatures;

        CPUFeatures features = CPUFeatures::NONE;

        //constexpr unsigned EAX = 0U;
        constexpr unsigned EBX = 1U;
        constexpr unsigned ECX = 2U;
        constexpr unsigned EDX = 3U;

        const auto max_ids = __get_cpuid_max(0x00000000U, nullptr);
        if (max_ids == 0) {
            // TODO: warn: cpuid not supported
            return features;
        }
        const auto max_extended_ids = __get_cpuid_max(0x80000000U, nullptr);

        /* get CPUID leaves for EAX = 1,7, and 0x80000001 */
        std::array<unsigned, 4> cpuid_leaf_1  = { 0U, 0U, 0U, 0U };
        std::array<unsigned, 4> cpuid_leaf_7  = { 0U, 0U, 0U, 0U };
        std::array<unsigned, 4> cpuid_leaf_e1 = { 0U, 0U, 0U, 0U };

        if (max_ids >= 1) {
            __get_cpuid(0x00000001U, &cpuid_leaf_1[0], &cpuid_leaf_1[1], &cpuid_leaf_1[2], &cpuid_leaf_1[3]);
        }
        if (max_ids >= 7) {
            __get_cpuid_count(0x00000007, 0x00000000U, &cpuid_leaf_7[0], &cpuid_leaf_7[1], &cpuid_leaf_7[2], &cpuid_leaf_7[3]);
        }
        if (max_extended_ids >= 0x80000001) {
            __get_cpuid(0x80000001U, &cpuid_leaf_e1[0], &cpuid_leaf_e1[1], &cpuid_leaf_e1[2], &cpuid_leaf_e1[3]);
        }

        if (cpuid_leaf_1[EDX] & bit_SSE   )     features |= CPUFeatures::CPU_FEATURE_SSE;
        if (cpuid_leaf_1[EDX] & bit_SSE2  )     features |= CPUFeatures::CPU_FEATURE_SSE2;
        if (cpuid_leaf_1[ECX] & bit_SSE3  )     features |= CPUFeatures::CPU_FEATURE_SSE3;
        if (cpuid_leaf_1[ECX] & bit_SSSE3 )     features |= CPUFeatures::CPU_FEATURE_SSSE3;
        if (cpuid_leaf_1[ECX] & bit_SSE4_1)     features |= CPUFeatures::CPU_FEATURE_SSE41;
        if (cpuid_leaf_1[ECX] & bit_SSE4_2)     features |= CPUFeatures::CPU_FEATURE_SSE42;
        if (cpuid_leaf_1[ECX] & bit_POPCNT)     features |= CPUFeatures::CPU_FEATURE_POPCNT;

        if (cpuid_leaf_1[ECX] & bit_AVX   )     features |= CPUFeatures::CPU_FEATURE_AVX;
        if (cpuid_leaf_1[ECX] & bit_F16C  )     features |= CPUFeatures::CPU_FEATURE_F16C;
        if (cpuid_leaf_1[ECX] & bit_RDRND)      features |= CPUFeatures::CPU_FEATURE_RDRAND;
        if (cpuid_leaf_7[EBX] & bit_AVX2  )     features |= CPUFeatures::CPU_FEATURE_AVX2;
        if (cpuid_leaf_1[ECX] & bit_FMA4  )     features |= CPUFeatures::CPU_FEATURE_FMA4;
        if (cpuid_leaf_e1[ECX] & bit_LZCNT)     features |= CPUFeatures::CPU_FEATURE_LZCNT;
        if (cpuid_leaf_7 [EBX] & bit_BMI )      features |= CPUFeatures::CPU_FEATURE_BMI1;
        if (cpuid_leaf_7 [EBX] & bit_BMI2 )     features |= CPUFeatures::CPU_FEATURE_BMI2;

        if (cpuid_leaf_7[EBX] & bit_AVX512F   ) features |= CPUFeatures::CPU_FEATURE_AVX512F;
        if (cpuid_leaf_7[EBX] & bit_AVX512DQ  ) features |= CPUFeatures::CPU_FEATURE_AVX512DQ;
        if (cpuid_leaf_7[EBX] & bit_AVX512PF  ) features |= CPUFeatures::CPU_FEATURE_AVX512PF;
        if (cpuid_leaf_7[EBX] & bit_AVX512ER  ) features |= CPUFeatures::CPU_FEATURE_AVX512ER;
        if (cpuid_leaf_7[EBX] & bit_AVX512CD  ) features |= CPUFeatures::CPU_FEATURE_AVX512CD;
        if (cpuid_leaf_7[EBX] & bit_AVX512BW  ) features |= CPUFeatures::CPU_FEATURE_AVX512BW;
        if (cpuid_leaf_7[EBX] & bit_AVX512IFMA) features |= CPUFeatures::CPU_FEATURE_AVX512IFMA;
        if (cpuid_leaf_7[EBX] & bit_AVX512VL  ) features |= CPUFeatures::CPU_FEATURE_AVX512VL;
        if (cpuid_leaf_7[ECX] & bit_AVX512VBMI) features |= CPUFeatures::CPU_FEATURE_AVX512VBMI;
        return features;
    }

    static void init()
    {
        s_vendor            = get_vendor();
        s_features          = get_cpu_features();
        s_atomic_sizes.a8   = check_atomic<8>();
        s_atomic_sizes.a16  = check_atomic<16>();
        s_atomic_sizes.a32  = check_atomic<32>();
        s_atomic_sizes.a64  = check_atomic<64>();
        s_atomic_sizes.a128 = check_atomic<128>();
    }

    struct AtomicSize
    {
        constexpr AtomicSize() noexcept
        : a8(false)
        , a16(false)
        , a32(false)
        , a64(false)
        , a128(false)
        {
        }

        bool a8   : 1;
        bool a16  : 1;
        bool a32  : 1;
        bool a64  : 1;
        bool a128 : 1;
    };

    static MOONRAY_MCRT_UTIL_EXPORT std::string s_vendor;
    static MOONRAY_MCRT_UTIL_EXPORT AtomicSize s_atomic_sizes;
    static MOONRAY_MCRT_UTIL_EXPORT cpuid_detail::CPUFeatures s_features;
};

} // namespace util
} // namespace moonray


#pragma warning pop

#endif 
