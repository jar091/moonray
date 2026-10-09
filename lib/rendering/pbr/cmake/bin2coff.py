#!/usr/bin/env python3
# Copyright 2026 DreamWorks Animation LLC
# SPDX-License-Identifier: Apache-2.0

"""Embed a binary file into a COFF (x86-64) object file.

This is the Windows/MSVC equivalent of

    objcopy --input-target=binary --binary-architecture=i386 \\
            --output-target=elf64-x86-64 <input.bin> <output.o>

which the sampler uses to link its pre-computed sample points straight into
the library.  GNU objcopy emits an ELF object with a `.data` section holding
the file verbatim plus the two boundary symbols `_binary_<mangled path>_start`
and `_binary_<mangled path>_end`; the C++ code declares those `extern "C"` and
takes their addresses.  There is no equivalent tool in the MSVC toolchain, so
this script writes the same thing in COFF form.  The symbol names are passed
in explicitly rather than re-derived, so the caller stays the single source of
truth for them.

Usage:
    bin2coff.py <input.bin> <output.obj> <start-symbol> <end-symbol>
"""

import struct
import sys

IMAGE_FILE_MACHINE_AMD64 = 0x8664

IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040
IMAGE_SCN_ALIGN_16BYTES = 0x00500000
IMAGE_SCN_MEM_READ = 0x40000000
IMAGE_SCN_MEM_WRITE = 0x80000000

IMAGE_SYM_CLASS_EXTERNAL = 2

FILE_HEADER_SIZE = 20
SECTION_HEADER_SIZE = 40
SYMBOL_SIZE = 18


def main(argv):
    if len(argv) != 5:
        sys.stderr.write(__doc__)
        return 2

    inputPath, outputPath, startSymbol, endSymbol = argv[1:5]

    with open(inputPath, 'rb') as f:
        data = f.read()

    # Both symbol names are far longer than eight characters, so they live in
    # the string table and the symbol record carries a zero "short name"
    # followed by the offset into it.  The string table starts with its own
    # four byte size field, hence the initial offset of 4.
    stringTable = bytearray(struct.pack('<I', 0))  # placeholder, patched below
    symbolOffsets = []
    for name in (startSymbol, endSymbol):
        symbolOffsets.append(len(stringTable))
        stringTable += name.encode('ascii') + b'\0'
    struct.pack_into('<I', stringTable, 0, len(stringTable))

    rawDataOffset = FILE_HEADER_SIZE + SECTION_HEADER_SIZE
    symbolTableOffset = rawDataOffset + len(data)

    out = bytearray()

    # IMAGE_FILE_HEADER
    out += struct.pack('<HHIIIHH',
                       IMAGE_FILE_MACHINE_AMD64,
                       1,                   # NumberOfSections
                       0,                   # TimeDateStamp (reproducible builds)
                       symbolTableOffset,
                       len(symbolOffsets),  # NumberOfSymbols
                       0,                   # SizeOfOptionalHeader
                       0)                   # Characteristics

    # IMAGE_SECTION_HEADER.  objcopy's ELF output puts the blob in a writable
    # .data section; mirroring that keeps the C++ side (which declares the
    # symbols non-const) valid on both toolchains.
    out += struct.pack('<8sIIIIIIHHI',
                       b'.data',
                       0,                   # VirtualSize
                       0,                   # VirtualAddress
                       len(data),           # SizeOfRawData
                       rawDataOffset,
                       0,                   # PointerToRelocations
                       0,                   # PointerToLinenumbers
                       0,                   # NumberOfRelocations
                       0,                   # NumberOfLinenumbers
                       IMAGE_SCN_CNT_INITIALIZED_DATA |
                       IMAGE_SCN_ALIGN_16BYTES |
                       IMAGE_SCN_MEM_READ |
                       IMAGE_SCN_MEM_WRITE)

    out += data

    # Symbol table: _start at offset 0 of the section, _end one past its last
    # byte.  x86-64 COFF does not decorate cdecl symbols, so the names are
    # exactly what `extern "C"` produces on the C++ side.
    for value, offset in zip((0, len(data)), symbolOffsets):
        out += struct.pack('<IIIhHBB',
                           0,               # Zeroes (long name form)
                           offset,          # Offset into the string table
                           value,           # Value
                           1,               # SectionNumber (1-based)
                           0,               # Type
                           IMAGE_SYM_CLASS_EXTERNAL,
                           0)               # NumberOfAuxSymbols

    out += stringTable

    with open(outputPath, 'wb') as f:
        f.write(out)

    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
