# Copyright 2026 DreamWorks Animation LLC
# SPDX-License-Identifier: Apache-2.0

# Cross-platform binary file concatenation for PMJ02 sampling chunks.
#
# Usage: cmake -DINPUT_FILES="file1;file2;file3" -DOUTPUT_FILE="output" -P ConcatenateBinaryFiles.cmake

if(NOT DEFINED INPUT_FILES OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "INPUT_FILES and OUTPUT_FILE must be specified")
endif()

# Convert space-separated INPUT_FILES string to a list if needed
if(NOT INPUT_FILES MATCHES ";")
    string(REPLACE " " ";" INPUT_FILES "${INPUT_FILES}")
endif()

# "cmake -E cat" concatenates in binary mode on every platform (CMake >= 3.18),
# so no per-platform branch is needed. It replaces a "cat" that only existed on
# Unix and a "cmd /c type" that could not work on Windows: "type" prints a
# banner line for each of several input files, which would corrupt the blob,
# and it cannot redirect to a forward-slash path.
execute_process(
    COMMAND ${CMAKE_COMMAND} -E cat ${INPUT_FILES}
    OUTPUT_FILE "${OUTPUT_FILE}"
    RESULT_VARIABLE result
)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "Failed to concatenate binary files: ${result}")
endif()
