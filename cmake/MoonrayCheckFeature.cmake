# Copyright 2025 DreamWorks Animation LLC
# SPDX-License-Identifier: Apache-2.0

#
# Testing __int128 atomic operation is lock-free or not and
# NO_16BYTE_ATOMIC_LOCK_FREE directive is defined if __int128 is not lock-free.
#

if(MSVC)
  # Neither __int128 nor the __atomic_* builtins exist here, so the probe below
  # cannot even be compiled - and the fallback it selects (scene_rdl2's
  # Atomic128.h) is GCC inline assembly, so it could not be used either.
  # moonray's own lib/common/mcrt_util/Atomic.h has a dedicated MSVC branch that
  # issues the very same `lock cmpxchg16b` through the
  # _InterlockedCompareExchange128 intrinsic, so 16-byte atomics are lock-free
  # here without the directive.
  message(STATUS "MSVC: 16-byte atomics use _InterlockedCompareExchange128 (mcrt_util/Atomic.h)")
else()
  file(WRITE "${CMAKE_BINARY_DIR}/checkInt128AtomicLockFree.cc"
  "
  #include <atomic>
  int main()
  {
      return (__atomic_is_lock_free(sizeof(__int128), 0x0)) ? 0 : 1;
  }
  ")

  message(STATUS "Using C++ compiler for try_run: ${CMAKE_CXX_COMPILER}")

  set_source_files_properties("${CMAKE_BINARY_DIR}/checkInt128AtomicLockFree.cc" PROPERTIES LANGUAGE CXX)
  try_run(ATOMIC128_RUN_RESULT ATOMIC128_COMPILE_RESULT
    "${CMAKE_BINARY_DIR}"
    "${CMAKE_BINARY_DIR}/checkInt128AtomicLockFree.cc"
    LINK_LIBRARIES atomic
  )

  # NOT <var> is the test for a failed try_run compile; `EQUAL FALSE` is a
  # numeric comparison against the non-numeric string "FALSE", which is never
  # true, so a failed compile used to fall through to the run test (whose result
  # is then "FAILED_TO_RUN") and silently select the fallback.
  if(NOT ATOMIC128_COMPILE_RESULT)
    message(WARNING "Compile failed for checking int128 atomic lock-free operation")
  else()
    if(ATOMIC128_RUN_RESULT EQUAL 0)
      message(STATUS "int128 atomic operation is lock-free")
    else()
      message(STATUS "int128 atomic operation is not lock-free, define NO_16BYTE_ATOMIC_LOCK_FREE directive")
      add_compile_definitions(NO_16BYTE_ATOMIC_LOCK_FREE)
    endif()
  endif()
endif()
