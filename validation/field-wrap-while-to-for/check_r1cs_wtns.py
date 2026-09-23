#!/usr/bin/env python3
"""Check LLZK-exported R1CS against its WTNS witness over BabyBear."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

BABYBEAR = 2013265921


def u32(data: bytes, offset: int) -> tuple[int, int]:
    return struct.unpack_from("<I", data, offset)[0], offset + 4


def u64(data: bytes, offset: int) -> tuple[int, int]:
    return struct.unpack_from("<Q", data, offset)[0], offset + 8


def sections(path: Path, magic: bytes) -> dict[int, bytes]:
    data = path.read_bytes()
    if data[:4] != magic:
        raise ValueError(f"{path}: expected {magic!r} header")
    version, offset = u32(data, 4)
    count, offset = u32(data, offset)
    if version not in (1, 2):
        raise ValueError(f"{path}: unsupported {magic.decode()} version {version}")
    result: dict[int, bytes] = {}
    for _ in range(count):
        section_id, offset = u32(data, offset)
        length, offset = u64(data, offset)
        end = offset + length
        if end > len(data) or section_id in result:
            raise ValueError(f"{path}: malformed section {section_id}")
        result[section_id] = data[offset:end]
        offset = end
    if offset != len(data):
        raise ValueError(f"{path}: trailing data after sections")
    return result


def field_element(data: bytes, offset: int, size: int) -> tuple[int, int]:
    end = offset + size
    if end > len(data):
        raise ValueError("truncated field element")
    return int.from_bytes(data[offset:end], "little"), end


def read_r1cs(path: Path) -> tuple[int, int, int, int, list[tuple[list[tuple[int, int]], ...]]]:
    data = sections(path, b"r1cs")
    if not {1, 2, 3}.issubset(data):
        raise ValueError(f"{path}: missing standard R1CS sections")
    header = data[1]
    field_size, offset = u32(header, 0)
    prime, offset = field_element(header, offset, field_size)
    wires, offset = u32(header, offset)
    public_outputs, offset = u32(header, offset)
    public_inputs, offset = u32(header, offset)
    private_inputs, offset = u32(header, offset)
    _, offset = u64(header, offset)  # number of wire labels
    constraint_count, offset = u32(header, offset)
    if offset != len(header):
        raise ValueError(f"{path}: malformed R1CS header")
    if prime != BABYBEAR:
        raise ValueError(f"{path}: expected BabyBear prime {BABYBEAR}, got {prime}")
    constraints = []
    body = data[2]
    offset = 0
    for _ in range(constraint_count):
        combinations = []
        for _ in range(3):
            terms, offset = u32(body, offset)
            combination = []
            for _ in range(terms):
                wire, offset = u32(body, offset)
                coefficient, offset = field_element(body, offset, field_size)
                if wire >= wires or coefficient >= prime:
                    raise ValueError(f"{path}: invalid R1CS term ({wire}, {coefficient})")
                combination.append((wire, coefficient))
            combinations.append(combination)
        constraints.append(tuple(combinations))
    if offset != len(body):
        raise ValueError(f"{path}: trailing constraint bytes")
    if len(data[3]) != wires * 8:
        raise ValueError(f"{path}: malformed wire-to-label section")
    return prime, wires, public_outputs, public_inputs + private_inputs, constraints


def read_wtns(path: Path) -> tuple[int, list[int]]:
    data = sections(path, b"wtns")
    if 1 not in data or 2 not in data:
        raise ValueError(f"{path}: missing WTNS header or values")
    header = data[1]
    field_size, offset = u32(header, 0)
    prime, offset = field_element(header, offset, field_size)
    count, offset = u32(header, offset)
    if offset != len(header) or len(data[2]) != count * field_size:
        raise ValueError(f"{path}: malformed WTNS sections")
    values = [
        int.from_bytes(data[2][i : i + field_size], "little")
        for i in range(0, len(data[2]), field_size)
    ]
    if any(value >= prime for value in values):
        raise ValueError(f"{path}: witness has a non-canonical field value")
    return prime, values


def failures(constraints: list[tuple[list[tuple[int, int]], ...]], values: list[int], prime: int) -> list[int]:
    failed = []
    for index, (a, b, c) in enumerate(constraints):
        eval_lc = lambda terms: sum(values[wire] * coefficient for wire, coefficient in terms) % prime
        if eval_lc(a) * eval_lc(b) % prime != eval_lc(c):
            failed.append(index)
    return failed


def validate(label: str, circuit_path: Path, witness_path: Path, expected: str) -> int:
    prime, wire_count, public_outputs, public_inputs, constraints = read_r1cs(circuit_path)
    witness_prime, values = read_wtns(witness_path)
    if witness_prime != prime or len(values) != wire_count:
        raise ValueError(f"{label}: witness prime/wire count does not match R1CS")
    if public_outputs != 1 or public_inputs != 0 or values != [1, 1]:
        raise ValueError(f"{label}: unexpected public layout or witness: {values}")
    failed = failures(constraints, values, prime)
    is_satisfied = not failed
    if (expected == "sat") != is_satisfied:
        raise ValueError(f"{label}: expected {expected}, failed R1CS constraints {failed}")
    print(
        f"{label}: witness={values}, constraints={len(constraints)}, "
        f"result={'SAT' if is_satisfied else f'UNSAT at {failed}'}"
    )
    return len(constraints)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--control", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--witness", type=Path, required=True)
    args = parser.parse_args()
    candidate = validate("wrapped candidate", args.candidate, args.witness, "sat")
    control = validate("no-wrap control", args.control, args.witness, "sat")
    reference = validate("source reference", args.reference, args.witness, "unsat")
    if candidate != 1 or control != 1 or reference != 2:
        raise ValueError(
            "unexpected constraint counts: expected candidate/control/reference = 1/1/2, "
            f"got {candidate}/{control}/{reference}"
        )
    print("BabyBear R1CS witness check passed; this is not a cryptographic proof.")


if __name__ == "__main__":
    main()
