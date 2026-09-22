#!/usr/bin/env python3
"""Check the comparison boundary in the lowered SMT dialect."""

from __future__ import annotations

import re
import sys
from pathlib import Path


def read(path: str) -> list[str]:
    return Path(path).read_text(encoding="utf-8").splitlines()


def value_for_constant(lines: list[str], value: str) -> str:
    pattern = re.compile(r"^(%[A-Za-z0-9_.]+) = smt\.int\.constant " + re.escape(value) + r"$")
    for line in lines:
        match = pattern.search(line.strip())
        if match:
            return match.group(1)
    raise AssertionError(f"missing SMT integer constant {value}")


def add_result(lines: list[str]) -> str:
    for line in lines:
        match = re.match(r"^\s*(%[A-Za-z0-9_.]+) = smt\.int\.add ", line)
        if match:
            return match.group(1)
    raise AssertionError("missing lowered field addition")


def has_direct_equality(lines: list[str], lhs: str, rhs: str) -> bool:
    needle = f"smt.eq {lhs}, {rhs}"
    reverse = f"smt.eq {rhs}, {lhs}"
    return any(needle in line or reverse in line for line in lines)


def assert_candidate(naive_path: str, optimized_path: str) -> None:
    naive = read(naive_path)
    optimized = read(optimized_path)

    naive_sum = add_result(naive)
    optimized_sum = add_result(optimized)
    naive_zero = value_for_constant(naive, "0")
    optimized_zero = value_for_constant(optimized, "0")

    if not has_direct_equality(naive, naive_sum, naive_zero):
        raise AssertionError(
            "naive lowering did not expose the expected raw sum == zero comparison"
        )
    if not has_direct_equality(optimized, optimized_sum, optimized_zero):
        raise AssertionError(
            "optimized lowering did not expose the same raw sum == zero comparison"
        )


def assert_control(control_path: str) -> None:
    control = read(control_path)
    control_sum = add_result(control)
    control_two = value_for_constant(control, "2")
    if not has_direct_equality(control, control_sum, control_two):
        raise AssertionError("control lowering did not preserve the ordinary comparison")


def assert_explicit_mod(control_path: str) -> None:
    control = read(control_path)
    zero = value_for_constant(control, "0")
    for line in control:
        match = re.match(r"^\s*(%[A-Za-z0-9_.]+) = smt\.int\.mod ", line)
        if match and has_direct_equality(control, match.group(1), zero):
            return
    raise AssertionError("explicit-mod control did not compare the canonicalized sum")


if __name__ == "__main__":
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: assert_ir.py candidate-naive.mlir candidate-optimized.mlir "
            "control-naive.mlir control-optimized.mlir explicit-mod-naive.mlir "
            "explicit-mod-optimized.mlir"
        )
    assert_candidate(sys.argv[1], sys.argv[2])
    assert_control(sys.argv[3])
    assert_control(sys.argv[4])
    assert_explicit_mod(sys.argv[5])
    assert_explicit_mod(sys.argv[6])
