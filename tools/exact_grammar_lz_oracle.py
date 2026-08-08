#!/usr/bin/env python3
"""
Exact small-block grammar -> LZ oracle.

The search enumerates every reduced straight-line grammar reachable by replacing
any chosen set of at least two non-overlapping occurrences of the same symbol
sequence. Rules are introduced bottom-up, so the grammar is always acyclic.
Single-use rules are inlined and isomorphic states are canonicalized.

For each grammar this tool tests every subset of rule expansions as a hidden LZ
prefix. Selected expansions are placed in an exact shortest-superstring layout
by enumerating every order after contained strings are removed. The resulting
prefix + output byte stream is parsed by dynamic programming using the AEGL
literal/copy command costs from grammar_lz.hpp.

This is an exact oracle for the grammar search and the LZ parse on short inputs.
The prefix search is exact over shortest-superstring layouts; deliberately
longer superstrings are not enumerated.
"""

from __future__ import annotations

import argparse
import csv
import itertools
import os
import sys
import time
import zlib
from dataclasses import dataclass
from functools import lru_cache
from typing import Iterable, Iterator, Sequence

TERMINAL_COUNT = 256
MAGIC = b"AEGL"
FLAGS = 0
EXTENDED_LITERAL = 0xC0
EXTENDED_COPY = 0xC1
SHORT_LITERAL_LIMIT = 128
SHORT_COPY_MINIMUM = 3
SHORT_COPY_MAXIMUM = 66

Layer = tuple[int, ...]
Rules = tuple[Layer, ...]
State = tuple[Layer, Rules]


def rule_symbol(index: int) -> int:
    return TERMINAL_COUNT + index


def is_rule(symbol: int) -> bool:
    return symbol >= TERMINAL_COUNT


def uvarint_size(value: int) -> int:
    size = 1
    while value >= 0x80:
        value >>= 7
        size += 1
    return size


def write_uvarint(out: bytearray, value: int) -> None:
    if value < 0:
        raise ValueError("uvarint cannot be negative")
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)


def read_uvarint(data: bytes, position: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while position < len(data):
        byte = data[position]
        position += 1
        value |= (byte & 0x7F) << shift
        if byte & 0x80 == 0:
            return value, position
        shift += 7
        if shift >= 64:
            raise ValueError("uvarint is too large")
    raise ValueError("truncated uvarint")


def grammar_size(state: State) -> int:
    root, rules = state
    return len(root) + sum(len(rule) for rule in rules)


def usage_counts(state: State) -> list[int]:
    root, rules = state
    counts = [0] * len(rules)
    for layer in (root, *rules):
        for symbol in layer:
            if is_rule(symbol):
                index = symbol - TERMINAL_COUNT
                if index >= len(rules):
                    raise ValueError("rule reference is out of range")
                counts[index] += 1
    return counts


def reachable_indices(state: State) -> set[int]:
    root, rules = state
    seen: set[int] = set()
    stack = [symbol - TERMINAL_COUNT for symbol in root if is_rule(symbol)]
    while stack:
        index = stack.pop()
        if index in seen:
            continue
        if index >= len(rules):
            raise ValueError("rule reference is out of range")
        seen.add(index)
        stack.extend(
            symbol - TERMINAL_COUNT
            for symbol in rules[index]
            if is_rule(symbol)
        )
    return seen


def remove_unreachable(state: State) -> State:
    root, rules = state
    reachable = reachable_indices(state)
    if len(reachable) == len(rules):
        return state

    order = sorted(reachable)
    old_to_new = {old: new for new, old in enumerate(order)}

    def remap(layer: Layer) -> Layer:
        result = []
        for symbol in layer:
            if is_rule(symbol):
                symbol = rule_symbol(old_to_new[symbol - TERMINAL_COUNT])
            result.append(symbol)
        return tuple(result)

    return remap(root), tuple(remap(rules[index]) for index in order)


def signature_height(signature: tuple) -> int:
    height = 0
    for token in signature:
        if token[0] == "n":
            height = max(height, 1 + signature_height(token[1]))
    return height


def structural_signatures(state: State) -> list[tuple]:
    _, rules = state

    @lru_cache(maxsize=None)
    def signature(index: int) -> tuple:
        result = []
        for symbol in rules[index]:
            if is_rule(symbol):
                child = symbol - TERMINAL_COUNT
                if child >= len(rules):
                    raise ValueError("rule reference is out of range")
                result.append(("n", signature(child)))
            else:
                result.append(("t", symbol))
        return tuple(result)

    return [signature(index) for index in range(len(rules))]


@lru_cache(maxsize=None)
def canonical_reindex_merge(state: State) -> State:
    state = remove_unreachable(state)
    root, rules = state
    if not rules:
        return state

    signatures = structural_signatures(state)
    unique = sorted(
        set(signatures),
        key=lambda signature: (signature_height(signature), signature),
    )
    signature_to_new = {
        signature: index for index, signature in enumerate(unique)
    }
    old_to_new = {
        index: signature_to_new[signature]
        for index, signature in enumerate(signatures)
    }
    representative: dict[tuple, int] = {}
    for index, signature in enumerate(signatures):
        representative.setdefault(signature, index)

    def remap(layer: Layer) -> Layer:
        result = []
        for symbol in layer:
            if is_rule(symbol):
                symbol = rule_symbol(old_to_new[symbol - TERMINAL_COUNT])
            result.append(symbol)
        return tuple(result)

    new_rules = tuple(
        remap(rules[representative[signature]]) for signature in unique
    )
    return remap(root), new_rules


def delete_and_inline(
    state: State, delete_index: int, replacement: Layer
) -> State:
    root, rules = state
    deleted_symbol = rule_symbol(delete_index)

    def substitute(layer: Layer) -> list[int]:
        result: list[int] = []
        for symbol in layer:
            if symbol == deleted_symbol:
                result.extend(replacement)
            else:
                result.append(symbol)
        return result

    new_root = substitute(root)
    new_rules = [
        substitute(rule)
        for index, rule in enumerate(rules)
        if index != delete_index
    ]

    def renumber(layer: Sequence[int]) -> Layer:
        result = []
        for symbol in layer:
            if is_rule(symbol):
                index = symbol - TERMINAL_COUNT
                if index > delete_index:
                    symbol -= 1
            result.append(symbol)
        return tuple(result)

    return renumber(new_root), tuple(renumber(rule) for rule in new_rules)


@lru_cache(maxsize=None)
def canonicalize(state: State) -> State:
    while True:
        state = canonical_reindex_merge(state)
        counts = usage_counts(state)
        single_use = [index for index, count in enumerate(counts) if count <= 1]
        if not single_use:
            return state
        index = single_use[-1]
        state = delete_and_inline(state, index, state[1][index])


@lru_cache(maxsize=None)
def expand_rule(state: State, index: int) -> bytes:
    _, rules = state
    result = bytearray()
    for symbol in rules[index]:
        if is_rule(symbol):
            result.extend(expand_rule(state, symbol - TERMINAL_COUNT))
        else:
            result.append(symbol)
    return bytes(result)


def expand_state(state: State) -> bytes:
    root, _ = state
    result = bytearray()
    for symbol in root:
        if is_rule(symbol):
            result.extend(expand_rule(state, symbol - TERMINAL_COUNT))
        else:
            result.append(symbol)
    return bytes(result)


def rule_expansions(state: State) -> tuple[bytes, ...]:
    return tuple(expand_rule(state, index) for index in range(len(state[1])))


@lru_cache(maxsize=None)
def nonoverlap_subsets(
    positions: tuple[int, ...], pattern_length: int
) -> tuple[tuple[int, ...], ...]:
    result: list[tuple[int, ...]] = []

    def visit(
        index: int, previous_end: int, selected: list[int]
    ) -> None:
        if index == len(positions):
            result.append(tuple(selected))
            return
        visit(index + 1, previous_end, selected)
        position = positions[index]
        if position >= previous_end:
            selected.append(position)
            visit(index + 1, position + pattern_length, selected)
            selected.pop()

    visit(0, -1, [])
    return tuple(result)


def replace_positions(
    layer: Layer,
    positions: tuple[int, ...],
    pattern_length: int,
    new_symbol: int,
) -> Layer:
    selected = set(positions)
    result = []
    position = 0
    while position < len(layer):
        if position in selected:
            result.append(new_symbol)
            position += pattern_length
        else:
            result.append(layer[position])
            position += 1
    return tuple(result)


def next_states(state: State) -> Iterator[State]:
    root, rules = state
    layers = (root, *rules)
    patterns: set[Layer] = set()
    for layer in layers:
        for length in range(2, len(layer) + 1):
            for position in range(len(layer) - length + 1):
                patterns.add(layer[position : position + length])

    new_symbol = rule_symbol(len(rules))
    emitted: set[State] = set()

    for pattern in sorted(patterns, key=lambda value: (len(value), value)):
        length = len(pattern)
        options: list[tuple[tuple[int, ...], ...]] = []
        maximum_count = 0
        for layer in layers:
            positions = tuple(
                position
                for position in range(len(layer) - length + 1)
                if layer[position : position + length] == pattern
            )
            layer_options = nonoverlap_subsets(positions, length)
            options.append(layer_options)
            maximum_count += max(
                (len(selection) for selection in layer_options), default=0
            )
        if maximum_count < 2:
            continue

        for selections in itertools.product(*options):
            if sum(len(selection) for selection in selections) < 2:
                continue
            new_layers = [
                replace_positions(layer, selection, length, new_symbol)
                for layer, selection in zip(layers, selections)
            ]
            candidate = canonicalize(
                (
                    new_layers[0],
                    tuple(new_layers[1:]) + (pattern,),
                )
            )
            if candidate != state and candidate not in emitted:
                emitted.add(candidate)
                yield candidate


@dataclass(frozen=True)
class GrammarSearch:
    states: tuple[State, ...]
    truncated: bool


def enumerate_grammars(
    data: bytes, maximum_states: int = 1_000_000
) -> GrammarSearch:
    start = canonicalize((tuple(data), tuple()))
    stack = [start]
    seen = {start}
    while stack:
        state = stack.pop()
        for candidate in next_states(state):
            if candidate in seen:
                continue
            seen.add(candidate)
            if len(seen) >= maximum_states:
                return GrammarSearch(tuple(seen), True)
            stack.append(candidate)
    return GrammarSearch(tuple(seen), False)


def literal_command_size(length: int) -> int:
    if length <= 0:
        raise ValueError("literal length must be positive")
    if length <= SHORT_LITERAL_LIMIT:
        return 1 + length
    return 1 + uvarint_size(length) + length


def copy_command_size(length: int, distance: int) -> int:
    if length < SHORT_COPY_MINIMUM or distance <= 0:
        return sys.maxsize
    if length <= SHORT_COPY_MAXIMUM:
        return 1 + uvarint_size(distance - 1)
    return 1 + uvarint_size(length) + uvarint_size(distance - 1)


@dataclass(frozen=True)
class LzOperation:
    is_copy: bool
    length: int
    distance: int = 0


@dataclass(frozen=True)
class LzParse:
    payload_size: int
    operations: tuple[LzOperation, ...]


@lru_cache(maxsize=None)
def exact_lz_parse(sequence: bytes) -> LzParse:
    size = len(sequence)
    infinity = sys.maxsize
    costs = [infinity] * (size + 1)
    operation_counts = [infinity] * (size + 1)
    choices: list[LzOperation | None] = [None] * (size + 1)
    costs[size] = 0
    operation_counts[size] = 0

    for position in range(size - 1, -1, -1):
        remaining = size - position

        for length in range(1, remaining + 1):
            cost = literal_command_size(length) + costs[position + length]
            count = 1 + operation_counts[position + length]
            key = (cost, count, 1, -length)
            current = (
                costs[position],
                operation_counts[position],
                2,
                0,
            )
            if key < current:
                costs[position] = cost
                operation_counts[position] = count
                choices[position] = LzOperation(False, length)

        for distance in range(1, position + 1):
            maximum = 0
            while (
                position + maximum < size
                and sequence[position + maximum]
                == sequence[position + maximum - distance]
            ):
                maximum += 1
            for length in range(SHORT_COPY_MINIMUM, maximum + 1):
                cost = (
                    copy_command_size(length, distance)
                    + costs[position + length]
                )
                count = 1 + operation_counts[position + length]
                key = (cost, count, 0, -length, distance)
                current_choice = choices[position]
                current = (
                    costs[position],
                    operation_counts[position],
                    0 if current_choice and current_choice.is_copy else 1,
                    -current_choice.length if current_choice else 0,
                    current_choice.distance if current_choice else 0,
                )
                if key < current:
                    costs[position] = cost
                    operation_counts[position] = count
                    choices[position] = LzOperation(True, length, distance)

    operations: list[LzOperation] = []
    position = 0
    while position < size:
        operation = choices[position]
        if operation is None:
            raise RuntimeError("failed to reconstruct optimal LZ parse")
        operations.append(operation)
        position += operation.length
    return LzParse(costs[0], tuple(operations))


def serialize_operations(
    sequence: bytes, operations: Sequence[LzOperation]
) -> bytes:
    output = bytearray()
    position = 0
    for operation in operations:
        if not operation.is_copy:
            if operation.length <= SHORT_LITERAL_LIMIT:
                output.append(operation.length - 1)
            else:
                output.append(EXTENDED_LITERAL)
                write_uvarint(output, operation.length)
            output.extend(sequence[position : position + operation.length])
        else:
            if operation.length <= SHORT_COPY_MAXIMUM:
                output.append(
                    0x80 + operation.length - SHORT_COPY_MINIMUM
                )
            else:
                output.append(EXTENDED_COPY)
                write_uvarint(output, operation.length)
            write_uvarint(output, operation.distance - 1)
        position += operation.length
    if position != len(sequence):
        raise ValueError("operations do not cover the sequence")
    return bytes(output)


def pack_frame(prefix: bytes, output: bytes, parse: LzParse) -> bytes:
    sequence = prefix + output
    commands = serialize_operations(sequence, parse.operations)
    if len(commands) != parse.payload_size:
        raise AssertionError("serialized LZ size differs from DP cost")
    frame = bytearray(MAGIC)
    frame.append(FLAGS)
    write_uvarint(frame, len(prefix))
    write_uvarint(frame, len(output))
    frame.extend(commands)
    return bytes(frame)


def decode_frame(frame: bytes) -> bytes:
    if len(frame) < 5 or frame[:4] != MAGIC or frame[4] != FLAGS:
        raise ValueError("bad AEGL header")
    position = 5
    prefix_size, position = read_uvarint(frame, position)
    output_size, position = read_uvarint(frame, position)
    total_size = prefix_size + output_size
    history = bytearray()

    while len(history) < total_size:
        if position >= len(frame):
            raise ValueError("truncated command stream")
        token = frame[position]
        position += 1

        if token <= 0x7F:
            length = token + 1
            end = position + length
            if end > len(frame) or len(history) + length > total_size:
                raise ValueError("bad literal command")
            history.extend(frame[position:end])
            position = end
            continue

        if 0x80 <= token <= 0xBF:
            length = SHORT_COPY_MINIMUM + token - 0x80
        elif token == EXTENDED_LITERAL:
            length, position = read_uvarint(frame, position)
            end = position + length
            if (
                length == 0
                or end > len(frame)
                or len(history) + length > total_size
            ):
                raise ValueError("bad extended literal")
            history.extend(frame[position:end])
            position = end
            continue
        elif token == EXTENDED_COPY:
            length, position = read_uvarint(frame, position)
        else:
            raise ValueError("unknown command token")

        encoded_distance, position = read_uvarint(frame, position)
        distance = encoded_distance + 1
        if (
            length < SHORT_COPY_MINIMUM
            or distance > len(history)
            or len(history) + length > total_size
        ):
            raise ValueError("bad copy command")
        for _ in range(length):
            history.append(history[-distance])

    if position != len(frame):
        raise ValueError("trailing bytes")
    return bytes(history[prefix_size:])


def maximum_overlap(left: bytes, right: bytes) -> int:
    for length in range(min(len(left), len(right)), 0, -1):
        if left[-length:] == right[:length]:
            return length
    return 0


def remove_contained(strings: Sequence[bytes]) -> tuple[bytes, ...]:
    ordered = sorted(set(strings), key=lambda value: (-len(value), value))
    kept: list[bytes] = []
    for value in ordered:
        if not any(value in existing for existing in kept):
            kept.append(value)
    return tuple(sorted(kept))


@lru_cache(maxsize=None)
def shortest_superstring_layouts(
    strings: tuple[bytes, ...]
) -> tuple[bytes, ...]:
    strings = remove_contained(strings)
    if not strings:
        return (b"",)

    layouts: set[bytes] = set()
    minimum_length: int | None = None
    for order in itertools.permutations(strings):
        result = order[0]
        for value in order[1:]:
            overlap = maximum_overlap(result, value)
            result += value[overlap:]
        if minimum_length is None or len(result) < minimum_length:
            minimum_length = len(result)
            layouts = {result}
        elif len(result) == minimum_length:
            layouts.add(result)
    return tuple(sorted(layouts))


@dataclass(frozen=True)
class PackedChoice:
    frame: bytes
    prefix: bytes
    selected_expansions: tuple[bytes, ...]
    parse: LzParse

    @property
    def frame_size(self) -> int:
        return len(self.frame)


@lru_cache(maxsize=None)
def best_packed_choice(
    output: bytes, expansions: tuple[bytes, ...]
) -> PackedChoice:
    unique = tuple(sorted(set(expansions)))
    best: PackedChoice | None = None

    for mask in range(1 << len(unique)):
        selected = tuple(
            unique[index]
            for index in range(len(unique))
            if mask & (1 << index)
        )
        for prefix in shortest_superstring_layouts(selected):
            parse = exact_lz_parse(prefix + output)
            frame = pack_frame(prefix, output, parse)
            candidate = PackedChoice(frame, prefix, selected, parse)
            key = (
                candidate.frame_size,
                len(prefix),
                len(selected),
                prefix,
            )
            if best is None:
                best = candidate
            else:
                best_key = (
                    best.frame_size,
                    len(best.prefix),
                    len(best.selected_expansions),
                    best.prefix,
                )
                if key < best_key:
                    best = candidate

    if best is None:
        raise AssertionError("packing search produced no candidate")
    if decode_frame(best.frame) != output:
        raise AssertionError("AEGL round trip failed")
    return best


@dataclass(frozen=True)
class OracleMetrics:
    data: bytes
    state_count: int
    truncated: bool
    minimum_grammar_size: int
    minimum_grammar_count: int
    plain: PackedChoice
    minimum_grammar: PackedChoice
    all_grammars: PackedChoice
    minimum_grammar_state: State
    all_grammars_state: State
    search_milliseconds: float


def state_sort_key(state: State) -> tuple:
    return grammar_size(state), state


@lru_cache(maxsize=None)
def oracle_metrics(
    data: bytes, maximum_states: int = 1_000_000
) -> OracleMetrics:
    begin = time.perf_counter()
    search = enumerate_grammars(data, maximum_states)
    if not search.states:
        raise AssertionError("grammar search produced no states")

    minimum_size = min(grammar_size(state) for state in search.states)
    minimum_states = [
        state
        for state in search.states
        if grammar_size(state) == minimum_size
    ]

    plain = best_packed_choice(data, tuple())

    minimum_choice: PackedChoice | None = None
    minimum_state: State | None = None
    all_choice = plain
    all_state = canonicalize((tuple(data), tuple()))

    for state in sorted(search.states, key=state_sort_key):
        choice = best_packed_choice(data, rule_expansions(state))
        if grammar_size(state) == minimum_size:
            key = (
                choice.frame_size,
                len(choice.prefix),
                state,
            )
            if minimum_choice is None:
                minimum_choice = choice
                minimum_state = state
            else:
                minimum_key = (
                    minimum_choice.frame_size,
                    len(minimum_choice.prefix),
                    minimum_state,
                )
                if key < minimum_key:
                    minimum_choice = choice
                    minimum_state = state

        all_key = (choice.frame_size, len(choice.prefix), state)
        current_key = (
            all_choice.frame_size,
            len(all_choice.prefix),
            all_state,
        )
        if all_key < current_key:
            all_choice = choice
            all_state = state

    if minimum_choice is None or minimum_state is None:
        raise AssertionError("minimum grammar choice was not selected")

    elapsed = (time.perf_counter() - begin) * 1000.0
    return OracleMetrics(
        data=data,
        state_count=len(search.states),
        truncated=search.truncated,
        minimum_grammar_size=minimum_size,
        minimum_grammar_count=len(minimum_states),
        plain=plain,
        minimum_grammar=minimum_choice,
        all_grammars=all_choice,
        minimum_grammar_state=minimum_state,
        all_grammars_state=all_state,
        search_milliseconds=elapsed,
    )


def symbol_text(symbol: int) -> str:
    if is_rule(symbol):
        return f"R{symbol - TERMINAL_COUNT}"
    if 32 <= symbol < 127 and chr(symbol) not in {"\\", ";"}:
        return chr(symbol)
    return f"\\x{symbol:02x}"


def format_layer(layer: Layer) -> str:
    return " ".join(symbol_text(symbol) for symbol in layer)


def format_state(state: State) -> str:
    root, rules = state
    parts = [f"root=[{format_layer(root)}]"]
    parts.extend(
        f"R{index}=[{format_layer(rule)}]"
        for index, rule in enumerate(rules)
    )
    return "; ".join(parts)


def raw_deflate_size(data: bytes) -> int:
    compressor = zlib.compressobj(
        level=9, method=zlib.DEFLATED, wbits=-zlib.MAX_WBITS
    )
    return len(compressor.compress(data) + compressor.flush())


def write_rows(fieldnames: Sequence[str], rows: Iterable[dict]) -> None:
    writer = csv.DictWriter(sys.stdout, fieldnames=fieldnames)
    writer.writeheader()
    for row in rows:
        writer.writerow(row)


def case_inputs() -> list[tuple[str, bytes]]:
    return [
        ("abc_example", b"abcabcdabcd"),
        ("first_hidden_prefix_win", b"abaabab"),
        ("two_byte_win", b"babaababab"),
        ("three_repeats", b"abcabcabc"),
        ("four_repeats", b"abcabcabcabc"),
        ("single_symbol_11", b"aaaaaaaaaaa"),
        ("fibonacci_11", b"abaababaaba"),
        ("json_fragment", b'{"device":"'),
    ]


def command_cases(_: argparse.Namespace) -> None:
    fields = [
        "case",
        "input_text",
        "input_hex",
        "input_bytes",
        "states",
        "truncated",
        "minimum_grammar_symbols",
        "minimum_grammar_count",
        "plain_payload_bytes",
        "minimum_grammar_payload_bytes",
        "all_grammars_payload_bytes",
        "plain_frame_bytes",
        "minimum_grammar_frame_bytes",
        "all_grammars_frame_bytes",
        "minimum_grammar_prefix_bytes",
        "all_grammars_prefix_bytes",
        "raw_deflate_bytes",
        "minimum_grammar",
        "all_grammars_best",
        "search_ms",
    ]
    rows = []
    for name, data in case_inputs():
        metrics = oracle_metrics(data)
        rows.append(
            {
                "case": name,
                "input_text": data.decode("latin1"),
                "input_hex": data.hex(),
                "input_bytes": len(data),
                "states": metrics.state_count,
                "truncated": int(metrics.truncated),
                "minimum_grammar_symbols": metrics.minimum_grammar_size,
                "minimum_grammar_count": metrics.minimum_grammar_count,
                "plain_payload_bytes": metrics.plain.parse.payload_size,
                "minimum_grammar_payload_bytes":
                    metrics.minimum_grammar.parse.payload_size,
                "all_grammars_payload_bytes":
                    metrics.all_grammars.parse.payload_size,
                "plain_frame_bytes": metrics.plain.frame_size,
                "minimum_grammar_frame_bytes":
                    metrics.minimum_grammar.frame_size,
                "all_grammars_frame_bytes":
                    metrics.all_grammars.frame_size,
                "minimum_grammar_prefix_bytes":
                    len(metrics.minimum_grammar.prefix),
                "all_grammars_prefix_bytes":
                    len(metrics.all_grammars.prefix),
                "raw_deflate_bytes": raw_deflate_size(data),
                "minimum_grammar":
                    format_state(metrics.minimum_grammar_state),
                "all_grammars_best":
                    format_state(metrics.all_grammars_state),
                "search_ms": f"{metrics.search_milliseconds:.3f}",
            }
        )
    write_rows(fields, rows)


def command_binary(arguments: argparse.Namespace) -> None:
    fields = [
        "length",
        "strings",
        "grammar_states",
        "maximum_states_for_one_string",
        "truncated_strings",
        "minimum_grammar_helped",
        "all_grammars_helped",
        "all_grammars_beat_minimum_grammar",
        "maximum_minimum_grammar_gain_bytes",
        "maximum_all_grammars_gain_bytes",
        "plain_frame_bytes_total",
        "minimum_grammar_frame_bytes_total",
        "all_grammars_frame_bytes_total",
        "elapsed_ms",
    ]
    rows = []
    for length in range(1, arguments.max_length + 1):
        begin = time.perf_counter()
        metrics_list = [
            oracle_metrics(bytes(value), arguments.maximum_states)
            for value in itertools.product(b"ab", repeat=length)
        ]
        rows.append(
            {
                "length": length,
                "strings": len(metrics_list),
                "grammar_states":
                    sum(value.state_count for value in metrics_list),
                "maximum_states_for_one_string":
                    max(value.state_count for value in metrics_list),
                "truncated_strings":
                    sum(value.truncated for value in metrics_list),
                "minimum_grammar_helped":
                    sum(
                        value.minimum_grammar.frame_size
                        < value.plain.frame_size
                        for value in metrics_list
                    ),
                "all_grammars_helped":
                    sum(
                        value.all_grammars.frame_size
                        < value.plain.frame_size
                        for value in metrics_list
                    ),
                "all_grammars_beat_minimum_grammar":
                    sum(
                        value.all_grammars.frame_size
                        < value.minimum_grammar.frame_size
                        for value in metrics_list
                    ),
                "maximum_minimum_grammar_gain_bytes":
                    max(
                        value.plain.frame_size
                        - value.minimum_grammar.frame_size
                        for value in metrics_list
                    ),
                "maximum_all_grammars_gain_bytes":
                    max(
                        value.plain.frame_size
                        - value.all_grammars.frame_size
                        for value in metrics_list
                    ),
                "plain_frame_bytes_total":
                    sum(value.plain.frame_size for value in metrics_list),
                "minimum_grammar_frame_bytes_total":
                    sum(
                        value.minimum_grammar.frame_size
                        for value in metrics_list
                    ),
                "all_grammars_frame_bytes_total":
                    sum(
                        value.all_grammars.frame_size
                        for value in metrics_list
                    ),
                "elapsed_ms":
                    f"{(time.perf_counter() - begin) * 1000.0:.3f}",
            }
        )
    write_rows(fields, rows)


def generated_iot_json() -> bytes:
    result = bytearray()
    for index in range(256):
        device = index % 16
        temperature = 18 + (index * 7) % 9
        fraction = (index * 37) % 100
        humidity = 30 + (index * 13) % 45
        battery = 2900 + (index * 17) % 350
        sequence = 100000 + index
        result.extend(
            (
                f'{{"device":"sensor-{device}",'
                f'"temperature":{temperature}.{fraction:02d},'
                f'"humidity":{humidity},'
                f'"battery_mv":{battery},'
                f'"sequence":{sequence}}}\n'
            ).encode()
        )
    return bytes(result)


def block_metrics(
    name: str,
    data: bytes,
    block_size: int,
    maximum_blocks: int,
    maximum_states: int,
) -> dict:
    blocks = [
        data[position : position + block_size]
        for position in range(0, len(data) - block_size + 1, block_size)
    ][:maximum_blocks]
    counts: dict[bytes, int] = {}
    for block in blocks:
        counts[block] = counts.get(block, 0) + 1

    calculated = {
        block: oracle_metrics(block, maximum_states) for block in counts
    }

    def weighted(selector) -> int:
        return sum(
            selector(calculated[block]) * count
            for block, count in counts.items()
        )

    return {
        "dataset": name,
        "block_size": block_size,
        "blocks": len(blocks),
        "unique_blocks": len(counts),
        "input_bytes": len(blocks) * block_size,
        "grammar_states_unique":
            sum(value.state_count for value in calculated.values()),
        "maximum_states_for_one_block":
            max((value.state_count for value in calculated.values()), default=0),
        "truncated_unique_blocks":
            sum(value.truncated for value in calculated.values()),
        "plain_payload_bytes":
            weighted(lambda value: value.plain.parse.payload_size),
        "minimum_grammar_payload_bytes":
            weighted(
                lambda value: value.minimum_grammar.parse.payload_size
            ),
        "all_grammars_payload_bytes":
            weighted(lambda value: value.all_grammars.parse.payload_size),
        "plain_frame_bytes":
            weighted(lambda value: value.plain.frame_size),
        "minimum_grammar_frame_bytes":
            weighted(lambda value: value.minimum_grammar.frame_size),
        "all_grammars_frame_bytes":
            weighted(lambda value: value.all_grammars.frame_size),
        "minimum_grammar_helped_blocks":
            sum(
                count
                for block, count in counts.items()
                if calculated[block].minimum_grammar.frame_size
                < calculated[block].plain.frame_size
            ),
        "all_grammars_helped_blocks":
            sum(
                count
                for block, count in counts.items()
                if calculated[block].all_grammars.frame_size
                < calculated[block].plain.frame_size
            ),
        "raw_deflate_bytes":
            sum(raw_deflate_size(block) * count for block, count in counts.items()),
    }


def command_blocks(arguments: argparse.Namespace) -> None:
    datasets = [("generated_iot_json", generated_iot_json())]
    for path in arguments.paths:
        with open(path, "rb") as file:
            datasets.append((os.path.basename(path), file.read()))

    fields = [
        "dataset",
        "block_size",
        "blocks",
        "unique_blocks",
        "input_bytes",
        "grammar_states_unique",
        "maximum_states_for_one_block",
        "truncated_unique_blocks",
        "plain_payload_bytes",
        "minimum_grammar_payload_bytes",
        "all_grammars_payload_bytes",
        "plain_frame_bytes",
        "minimum_grammar_frame_bytes",
        "all_grammars_frame_bytes",
        "minimum_grammar_helped_blocks",
        "all_grammars_helped_blocks",
        "raw_deflate_bytes",
    ]
    rows = [
        block_metrics(
            name,
            data,
            arguments.block_size,
            arguments.max_blocks,
            arguments.maximum_states,
        )
        for name, data in datasets
    ]
    write_rows(fields, rows)


def command_self_test(_: argparse.Namespace) -> None:
    example = oracle_metrics(b"abcabcdabcd")
    assert not example.truncated
    assert example.minimum_grammar_size == 8
    assert example.minimum_grammar_count == 2
    assert example.plain.frame_size == 17
    assert example.minimum_grammar.frame_size == 17
    assert example.all_grammars.frame_size == 17

    hidden_prefix_win = oracle_metrics(b"abaabab")
    assert hidden_prefix_win.plain.frame_size == 15
    assert hidden_prefix_win.minimum_grammar.frame_size == 14
    assert hidden_prefix_win.all_grammars.frame_size == 14
    assert hidden_prefix_win.all_grammars.prefix == b"ab"

    for _, data in case_inputs():
        metrics = oracle_metrics(data)
        for choice in (
            metrics.plain,
            metrics.minimum_grammar,
            metrics.all_grammars,
        ):
            assert decode_frame(choice.frame) == data
            assert len(choice.frame) == choice.frame_size

    print("exact grammar-lz oracle tests passed")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    self_test = subparsers.add_parser("self-test")
    self_test.set_defaults(function=command_self_test)

    cases = subparsers.add_parser("cases")
    cases.set_defaults(function=command_cases)

    binary = subparsers.add_parser("binary")
    binary.add_argument("--max-length", type=int, default=10)
    binary.add_argument("--maximum-states", type=int, default=1_000_000)
    binary.set_defaults(function=command_binary)

    blocks = subparsers.add_parser("blocks")
    blocks.add_argument("paths", nargs="*")
    blocks.add_argument("--block-size", type=int, default=11)
    blocks.add_argument("--max-blocks", type=int, default=128)
    blocks.add_argument("--maximum-states", type=int, default=1_000_000)
    blocks.set_defaults(function=command_blocks)

    return parser


def main() -> int:
    arguments = build_parser().parse_args()
    arguments.function(arguments)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
