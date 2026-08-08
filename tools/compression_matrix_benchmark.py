#!/usr/bin/env python3
"""Compression matrix for exact short-block Grammar->LZ and standard codecs."""

from __future__ import annotations

import argparse
import csv
import lzma
import random
import sys
import zlib
from collections import Counter
from pathlib import Path
from typing import Iterable

import brotli
import lz4.block
import lz4.frame
import zstandard

from exact_grammar_lz_oracle import (
    best_packed_choice,
    exact_lz_parse,
    oracle_metrics,
    shortest_superstring_layouts,
    uvarint_size,
)

TARGET_SIZE = 8192


def repeat_to_size(data: bytes, size: int = TARGET_SIZE) -> bytes:
    if not data:
        return bytes(size)
    return (data * ((size + len(data) - 1) // len(data)))[:size]


def generated_iot_json(size: int = TARGET_SIZE) -> bytes:
    lines: list[str] = []
    index = 0
    while sum(len(line) for line in lines) < size + 256:
        lines.append(
            '{"device":"sensor-%02d","temperature":%d.%02d,'
            '"humidity":%d,"battery_mv":%d,"sequence":%d}\n'
            % (
                index % 32,
                18 + (index * 7) % 9,
                (index * 37) % 100,
                30 + (index * 13) % 45,
                2900 + (index * 17) % 350,
                100000 + index,
            )
        )
        index += 1
    return "".join(lines).encode()[:size]


def generated_logs(size: int = TARGET_SIZE) -> bytes:
    levels = ("INFO", "DEBUG", "WARN", "INFO", "INFO", "ERROR")
    components = ("radio", "sensor", "transport", "storage", "scheduler")
    lines: list[str] = []
    index = 0
    while sum(len(line) for line in lines) < size + 256:
        lines.append(
            "2026-08-08T14:%02d:%02d.%03dZ %-5s device=%02d component=%s "
            "sequence=%06d value=%d status=%s\n"
            % (
                (index // 60) % 60,
                index % 60,
                (index * 137) % 1000,
                levels[index % len(levels)],
                index % 32,
                components[index % len(components)],
                index,
                (index * 97) % 10000,
                "ok" if index % 17 else "retry",
            )
        )
        index += 1
    return "".join(lines).encode()[:size]


def generated_csv(size: int = TARGET_SIZE) -> bytes:
    rows = ["timestamp,device,temperature,humidity,battery_mv,sequence\n"]
    index = 0
    while sum(len(row) for row in rows) < size + 256:
        rows.append(
            "%d,sensor-%02d,%d.%02d,%d,%d,%d\n"
            % (
                1786200000 + index * 60,
                index % 32,
                18 + (index * 7) % 9,
                (index * 37) % 100,
                30 + (index * 13) % 45,
                2900 + (index * 17) % 350,
                index,
            )
        )
        index += 1
    return "".join(rows).encode()[:size]


def generated_utf8(size: int = TARGET_SIZE) -> bytes:
    text = (
        "Телеметрия устройства передаётся редко, поэтому каждый байт заголовка "
        "заметен. Компрессор строит иерархию повторяющихся фрагментов, а "
        "декодер восстанавливает данные последовательно. "
    ).encode("utf-8")
    return repeat_to_size(text, size)


def generated_repeating(size: int = TARGET_SIZE) -> bytes:
    pattern = (
        b"sensor.temperature=21.25;sensor.humidity=40;device=alpha;"
        b"state=online;sequence=000001\n"
    )
    return repeat_to_size(pattern, size)


def generated_low_entropy(size: int = TARGET_SIZE) -> bytes:
    out = bytearray()
    for index in range(size):
        out.append(((index // 17) ^ (index // 61) ^ (index % 5)) & 0x0F)
    return bytes(out)


def generated_random(size: int = TARGET_SIZE) -> bytes:
    rng = random.Random(0xA37E2026)
    return bytes(rng.randrange(256) for _ in range(size))


def read_or_repeat(path: Path, size: int = TARGET_SIZE) -> bytes:
    return repeat_to_size(path.read_bytes(), size)


def build_datasets(source_path: Path, binary_path: Path) -> dict[str, bytes]:
    return {
        "repeating_text": generated_repeating(),
        "iot_json": generated_iot_json(),
        "logs": generated_logs(),
        "csv": generated_csv(),
        "utf8_text": generated_utf8(),
        "source_cpp": read_or_repeat(source_path),
        "elf_binary": read_or_repeat(binary_path),
        "low_entropy_binary": generated_low_entropy(),
        "random": generated_random(),
    }


def write_datasets(directory: Path, datasets: dict[str, bytes]) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    for name, data in datasets.items():
        (directory / f"{name}.bin").write_bytes(data)


def raw_deflate(data: bytes) -> bytes:
    compressor = zlib.compressobj(level=9, wbits=-15)
    result = compressor.compress(data) + compressor.flush()
    assert zlib.decompress(result, wbits=-15) == data
    return result


def xz_64k(data: bytes) -> bytes:
    filters = [{"id": lzma.FILTER_LZMA2, "dict_size": 65536}]
    result = lzma.compress(data, format=lzma.FORMAT_XZ, filters=filters)
    assert lzma.decompress(result, format=lzma.FORMAT_XZ) == data
    return result


def codec_sizes(data: bytes) -> dict[str, int]:
    raw = raw_deflate(data)
    zlib_frame = zlib.compress(data, level=9)
    zstd_frame = zstandard.ZstdCompressor(level=19).compress(data)
    brotli_stream = brotli.compress(data, quality=11)
    lz4_payload = lz4.block.compress(
        data, mode="high_compression", compression=12, store_size=False
    )
    lz4_frame = lz4.frame.compress(
        data,
        compression_level=16,
        block_linked=True,
        store_size=True,
    )
    xz_small = xz_64k(data)
    xz_default = lzma.compress(data, format=lzma.FORMAT_XZ, preset=6)

    assert zlib.decompress(zlib_frame) == data
    assert zstandard.ZstdDecompressor().decompress(zstd_frame) == data
    assert brotli.decompress(brotli_stream) == data
    assert lz4.block.decompress(lz4_payload, uncompressed_size=len(data)) == data
    assert lz4.frame.decompress(lz4_frame) == data
    assert lzma.decompress(xz_default) == data

    return {
        "raw_deflate_payload": len(raw),
        "raw_deflate_framed": len(raw) + uvarint_size(len(data)),
        "zlib_frame": len(zlib_frame),
        "zstd_frame": len(zstd_frame),
        "brotli_stream": len(brotli_stream),
        "lz4_block_framed": len(lz4_payload) + uvarint_size(len(data)),
        "lz4_frame": len(lz4_frame),
        "xz_64k_frame": len(xz_small),
        "xz_default_frame": len(xz_default),
    }


def clear_oracle_caches() -> None:
    oracle_metrics.cache_clear()
    exact_lz_parse.cache_clear()
    best_packed_choice.cache_clear()
    shortest_superstring_layouts.cache_clear()


def select_blocks(data: bytes, block_size: int, maximum: int) -> list[bytes]:
    count = len(data) // block_size
    if count == 0:
        return []
    if count <= maximum:
        indices = list(range(count))
    elif maximum <= 1:
        indices = [0]
    else:
        indices = sorted(
            {
                round(position * (count - 1) / (maximum - 1))
                for position in range(maximum)
            }
        )
    return [
        data[index * block_size : (index + 1) * block_size]
        for index in indices
    ]


def benchmark_blocks(
    datasets: dict[str, bytes],
    block_sizes: Iterable[int],
    maximum_blocks: int,
    maximum_states: int,
) -> None:
    columns = [
        "dataset",
        "block_size",
        "blocks",
        "unique_blocks",
        "input_bytes",
        "grammar_states_unique",
        "maximum_states_for_one_block",
        "truncated_unique_blocks",
        "aegl_plain_payload",
        "aegl_minimum_grammar_payload",
        "aegl_best_grammar_payload",
        "aegl_plain_frame",
        "aegl_minimum_grammar_frame",
        "aegl_best_grammar_frame",
        "minimum_grammar_helped_blocks",
        "best_grammar_helped_blocks",
        "nonminimum_grammar_won_blocks",
        "maximum_minimum_gain",
        "maximum_best_gain",
        "raw_deflate_payload",
        "raw_deflate_framed",
        "zlib_frame",
        "zstd_frame",
        "brotli_stream",
        "lz4_block_framed",
        "lz4_frame",
        "xz_64k_frame",
        "xz_default_frame",
        "best_standard_codec",
        "best_standard_bytes",
    ]
    writer = csv.DictWriter(sys.stdout, fieldnames=columns)
    writer.writeheader()

    standard_names = (
        "raw_deflate_framed",
        "zlib_frame",
        "zstd_frame",
        "brotli_stream",
        "lz4_block_framed",
        "lz4_frame",
        "xz_64k_frame",
        "xz_default_frame",
    )

    for dataset, data in datasets.items():
        for block_size in block_sizes:
            blocks = select_blocks(data, block_size, maximum_blocks)
            counts = Counter(blocks)
            totals = {name: 0 for name in codec_sizes(b"").keys()}
            plain_payload = 0
            minimum_payload = 0
            best_payload = 0
            plain_frame = 0
            minimum_frame = 0
            best_frame = 0
            states_total = 0
            states_max = 0
            truncated = 0
            minimum_helped = 0
            best_helped = 0
            nonminimum_won = 0
            maximum_minimum_gain = 0
            maximum_best_gain = 0

            for block, multiplicity in counts.items():
                metrics = oracle_metrics(block, maximum_states)
                sizes = codec_sizes(block)
                for name, value in sizes.items():
                    totals[name] += value * multiplicity

                states_total += metrics.state_count
                states_max = max(states_max, metrics.state_count)
                if metrics.truncated:
                    truncated += 1

                plain_payload += metrics.plain.parse.payload_size * multiplicity
                minimum_payload += (
                    metrics.minimum_grammar.parse.payload_size * multiplicity
                )
                best_payload += metrics.all_grammars.parse.payload_size * multiplicity
                plain_frame += metrics.plain.frame_size * multiplicity
                minimum_frame += metrics.minimum_grammar.frame_size * multiplicity
                best_frame += metrics.all_grammars.frame_size * multiplicity

                minimum_gain = (
                    metrics.plain.frame_size
                    - metrics.minimum_grammar.frame_size
                )
                best_gain = metrics.plain.frame_size - metrics.all_grammars.frame_size
                if minimum_gain > 0:
                    minimum_helped += multiplicity
                if best_gain > 0:
                    best_helped += multiplicity
                if metrics.all_grammars.frame_size < metrics.minimum_grammar.frame_size:
                    nonminimum_won += multiplicity
                maximum_minimum_gain = max(maximum_minimum_gain, minimum_gain)
                maximum_best_gain = max(maximum_best_gain, best_gain)
                clear_oracle_caches()

            best_standard_codec = min(standard_names, key=lambda name: totals[name])
            row = {
                "dataset": dataset,
                "block_size": block_size,
                "blocks": len(blocks),
                "unique_blocks": len(counts),
                "input_bytes": len(blocks) * block_size,
                "grammar_states_unique": states_total,
                "maximum_states_for_one_block": states_max,
                "truncated_unique_blocks": truncated,
                "aegl_plain_payload": plain_payload,
                "aegl_minimum_grammar_payload": minimum_payload,
                "aegl_best_grammar_payload": best_payload,
                "aegl_plain_frame": plain_frame,
                "aegl_minimum_grammar_frame": minimum_frame,
                "aegl_best_grammar_frame": best_frame,
                "minimum_grammar_helped_blocks": minimum_helped,
                "best_grammar_helped_blocks": best_helped,
                "nonminimum_grammar_won_blocks": nonminimum_won,
                "maximum_minimum_gain": maximum_minimum_gain,
                "maximum_best_gain": maximum_best_gain,
                **totals,
                "best_standard_codec": best_standard_codec,
                "best_standard_bytes": totals[best_standard_codec],
            }
            writer.writerow(row)


def benchmark_files(directory: Path) -> None:
    columns = [
        "dataset",
        "input_bytes",
        "raw_deflate_payload",
        "raw_deflate_framed",
        "zlib_frame",
        "zstd_frame",
        "brotli_stream",
        "lz4_block_framed",
        "lz4_frame",
        "xz_64k_frame",
        "xz_default_frame",
        "best_standard_codec",
        "best_standard_bytes",
    ]
    writer = csv.DictWriter(sys.stdout, fieldnames=columns)
    writer.writeheader()
    standard_names = tuple(columns[2:-2])
    for path in sorted(directory.glob("*.bin")):
        data = path.read_bytes()
        sizes = codec_sizes(data)
        best = min(standard_names, key=lambda name: sizes[name])
        writer.writerow(
            {
                "dataset": path.stem,
                "input_bytes": len(data),
                **sizes,
                "best_standard_codec": best,
                "best_standard_bytes": sizes[best],
            }
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    generate = subparsers.add_parser("generate")
    generate.add_argument("source", type=Path)
    generate.add_argument("binary", type=Path)
    generate.add_argument("output", type=Path)

    blocks = subparsers.add_parser("blocks")
    blocks.add_argument("source", type=Path)
    blocks.add_argument("binary", type=Path)
    blocks.add_argument("--block-sizes", default="8,10,11")
    blocks.add_argument("--max-blocks", type=int, default=64)
    blocks.add_argument("--max-states", type=int, default=1_000_000)

    files = subparsers.add_parser("files")
    files.add_argument("directory", type=Path)

    arguments = parser.parse_args()
    if arguments.command == "generate":
        write_datasets(
            arguments.output,
            build_datasets(arguments.source, arguments.binary),
        )
        return 0
    if arguments.command == "blocks":
        sizes = tuple(
            int(value) for value in arguments.block_sizes.split(",") if value
        )
        benchmark_blocks(
            build_datasets(arguments.source, arguments.binary),
            sizes,
            arguments.max_blocks,
            arguments.max_states,
        )
        return 0
    if arguments.command == "files":
        benchmark_files(arguments.directory)
        return 0
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
