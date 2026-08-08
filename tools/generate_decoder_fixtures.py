#!/usr/bin/env python3
"""Generate deterministic decoder footprint fixtures."""

from __future__ import annotations

import argparse
import csv
import lzma
import zlib
from pathlib import Path

import brotli
import lz4.frame
import zstandard

from compression_matrix_benchmark import (
    generated_iot_json,
    generated_logs,
    generated_low_entropy,
    generated_random,
    generated_repeating,
    generated_utf8,
)


def make_fixture(size: int) -> bytes:
    parts = [
        generated_iot_json(size),
        generated_logs(size),
        generated_repeating(size),
        generated_utf8(size),
        generated_low_entropy(size),
        generated_random(size),
    ]
    output = bytearray()
    position = 0
    while len(output) < size:
        part = parts[position % len(parts)]
        take = min(4096, size - len(output))
        start = (position * 997) % (len(part) - take + 1)
        output.extend(part[start : start + take])
        position += 1
    return bytes(output)


def raw_deflate(data: bytes) -> bytes:
    compressor = zlib.compressobj(level=9, wbits=-15)
    result = compressor.compress(data) + compressor.flush()
    assert zlib.decompress(result, wbits=-15) == data
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--size", type=int, default=65536)
    arguments = parser.parse_args()

    arguments.output.mkdir(parents=True, exist_ok=True)
    original = make_fixture(arguments.size)
    fixtures: dict[str, bytes] = {
        "raw_deflate": raw_deflate(original),
        "zstd_19": zstandard.ZstdCompressor(level=19).compress(original),
        "brotli_11": brotli.compress(original, quality=11),
        "xz_default_6": lzma.compress(
            original, format=lzma.FORMAT_XZ, preset=6
        ),
        "xz_64k": lzma.compress(
            original,
            format=lzma.FORMAT_XZ,
            filters=[{"id": lzma.FILTER_LZMA2, "dict_size": 65536}],
        ),
        "lz4_frame": lz4.frame.compress(
            original,
            compression_level=16,
            block_linked=True,
            store_size=True,
        ),
    }

    assert zstandard.ZstdDecompressor().decompress(fixtures["zstd_19"]) == original
    assert brotli.decompress(fixtures["brotli_11"]) == original
    assert lzma.decompress(fixtures["xz_default_6"]) == original
    assert lzma.decompress(fixtures["xz_64k"]) == original
    assert lz4.frame.decompress(fixtures["lz4_frame"]) == original

    (arguments.output / "original.bin").write_bytes(original)
    for name, data in fixtures.items():
        (arguments.output / f"{name}.bin").write_bytes(data)

    writer = csv.writer(sys.stdout)
    writer.writerow(("fixture", "compressed_bytes", "output_bytes"))
    for name, data in fixtures.items():
        writer.writerow((name, len(data), len(original)))
    return 0


if __name__ == "__main__":
    import sys

    raise SystemExit(main())
