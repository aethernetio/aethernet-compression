#!/usr/bin/env python3
from __future__ import annotations

import lzma
from pathlib import Path
import zlib

import brotli
import zstandard as zstd


def raw_deflate(data: bytes) -> bytes:
    compressor = zlib.compressobj(level=9, wbits=-15)
    return compressor.compress(data) + compressor.flush()


print("dataset,input_bytes,variant,compressed_bytes")
for path in sorted(Path("benchmark-data").glob("*.bin")):
    data = path.read_bytes()
    results = {
        "raw_deflate_9": len(raw_deflate(data)),
        "zlib_9": len(zlib.compress(data, 9)),
        "zstd_19": len(zstd.ZstdCompressor(level=19).compress(data)),
        "brotli_11": len(brotli.compress(data, quality=11)),
        "xz_9e": len(lzma.compress(data, preset=9 | lzma.PRESET_EXTREME)),
    }
    for variant, size in results.items():
        print(f"{path.stem},{len(data)},{variant},{size}")
