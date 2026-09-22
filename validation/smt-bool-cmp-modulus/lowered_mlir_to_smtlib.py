#!/usr/bin/env python3
"""Translate this reproducer's lowered SMT IR subset to standard SMT-LIB.

The v2.1.2 source emits the SMT dialect from llzk-opt but predates the
llzk-translate and llzk-smt-check executables. This fail-closed bridge keeps
the exact release lowering output as input. Its complete output is compared
with the official main translator for the same six cases in CI.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


VALUE = r"%[A-Za-z0-9_.]+"


def region(lines: list[str], start: int) -> tuple[list[str], int]:
    """Return one brace-delimited MLIR region, rejecting unbalanced input."""
    depth = 0
    body: list[str] = []
    for index in range(start, len(lines)):
        line = lines[index]
        depth += line.count("{") - line.count("}")
        body.append(line)
        if depth == 0:
            return body, index + 1
    raise ValueError("unterminated MLIR region")


def lookup(env: dict[str, str], value: str) -> str:
    try:
        return env[value]
    except KeyError as error:
        raise ValueError(f"use of undefined SSA value {value}") from error


def evaluate_function(
    lines: list[str], arguments: list[str]
) -> list[str]:
    header = re.fullmatch(
        r"\s*func\.func @([A-Za-z0-9_.$-]+)\((.*)\) \{", lines[0]
    )
    if not header:
        raise ValueError("malformed lowered function header")
    parameters = re.findall(rf"({VALUE}): !smt\.int", header.group(2))
    if len(parameters) != len(arguments):
        raise ValueError("function argument count does not match call")
    env = dict(zip(parameters, arguments, strict=True))
    assertions: list[str] = []

    for raw in lines[1:-1]:
        line = raw.strip()
        match = re.fullmatch(rf"({VALUE}) = smt\.int\.constant (-?[0-9]+)", line)
        if match:
            env[match.group(1)] = match.group(2)
            continue
        match = re.fullmatch(rf"({VALUE}) = smt\.int\.add ({VALUE}), ({VALUE})", line)
        if match:
            lhs = lookup(env, match.group(2))
            rhs = lookup(env, match.group(3))
            env[match.group(1)] = f"(+ {lhs} {rhs})"
            continue
        match = re.fullmatch(rf"({VALUE}) = smt\.int\.mod ({VALUE}), ({VALUE})", line)
        if match:
            lhs = lookup(env, match.group(2))
            rhs = lookup(env, match.group(3))
            env[match.group(1)] = f"(mod {lhs} {rhs})"
            continue
        match = re.fullmatch(
            rf"({VALUE}) = smt\.eq ({VALUE}), ({VALUE}) : !smt\.int", line
        )
        if match:
            lhs = lookup(env, match.group(2))
            rhs = lookup(env, match.group(3))
            env[match.group(1)] = f"(= {lhs} {rhs})"
            continue
        match = re.fullmatch(
            rf"({VALUE}) = smt\.int\.cmp (ge|gt|le|lt) ({VALUE}), ({VALUE})", line
        )
        if match:
            op = {"ge": ">=", "gt": ">", "le": "<=", "lt": "<"}[match.group(2)]
            lhs = lookup(env, match.group(3))
            rhs = lookup(env, match.group(4))
            env[match.group(1)] = f"({op} {lhs} {rhs})"
            continue
        match = re.fullmatch(rf"({VALUE}) = smt\.not ({VALUE})", line)
        if match:
            env[match.group(1)] = f"(not {lookup(env, match.group(2))})"
            continue
        match = re.fullmatch(
            rf"({VALUE}) = smt\.ite ({VALUE}), ({VALUE}), ({VALUE}) : !smt\.int",
            line,
        )
        if match:
            env[match.group(1)] = (
                f"(ite {lookup(env, match.group(2))} "
                f"{lookup(env, match.group(3))} {lookup(env, match.group(4))})"
            )
            continue
        cast_pattern = (
            rf"({VALUE}) = builtin\.unrealized_conversion_cast ({VALUE}) : "
            r"!smt\.bool to i1"
        )
        match = re.fullmatch(cast_pattern, line)
        if match:
            env[match.group(1)] = lookup(env, match.group(2))
            continue
        match = re.fullmatch(rf"smt\.assert ({VALUE})", line)
        if match:
            assertions.append(f"(assert {lookup(env, match.group(1))})")
            continue
        if line == "return":
            continue
        raise ValueError(f"unsupported lowered function operation: {line}")

    return assertions


def translate(path: Path) -> str:
    lines = path.read_text(encoding="utf-8").splitlines()
    function: list[str] | None = None
    solver: list[str] | None = None
    index = 0
    while index < len(lines):
        if re.match(r"\s*func\.func @", lines[index]):
            if function is not None:
                raise ValueError("expected exactly one lowered function")
            function, index = region(lines, index)
            continue
        if re.match(r"\s*smt\.solver .*\{", lines[index]):
            if solver is not None:
                raise ValueError("expected exactly one SMT solver root")
            solver, index = region(lines, index)
            continue
        index += 1
    if function is None or solver is None:
        raise ValueError("expected one lowered function and one SMT solver root")

    output = ["(set-logic ALL)"]
    env: dict[str, str] = {}
    assertions: list[str] = []
    call_count = 0
    check_count = 0

    for raw in solver[1:-1]:
        line = raw.strip()
        match = re.fullmatch(r'smt\.set_info "(:[A-Za-z0-9_-]+)" "([^"]*)"', line)
        if match:
            output.append(f'(set-info {match.group(1)} "{match.group(2)}")')
            continue
        match = re.fullmatch(
            r'smt\.set_info "(:[A-Za-z0-9_-]+)" (sat|unsat|unknown)', line
        )
        if match:
            output.append(f"(set-info {match.group(1)} {match.group(2)})")
            continue
        match = re.fullmatch(
            rf'({VALUE}) = smt\.declare_fun "([^"]+)" : !smt\.int', line
        )
        if match:
            env[match.group(1)] = match.group(2)
            output.append(f"(declare-fun {match.group(2)} () Int)")
            continue
        match = re.fullmatch(rf"({VALUE}) = smt\.int\.constant (-?[0-9]+)", line)
        if match:
            env[match.group(1)] = match.group(2)
            continue
        match = re.fullmatch(
            rf"({VALUE}) = smt\.eq ({VALUE}), ({VALUE}) : !smt\.int", line
        )
        if match:
            lhs = lookup(env, match.group(2))
            rhs = lookup(env, match.group(3))
            env[match.group(1)] = f"(= {lhs} {rhs})"
            continue
        match = re.fullmatch(rf"smt\.assert ({VALUE})", line)
        if match:
            assertions.append(f"(assert {lookup(env, match.group(1))})")
            continue
        match = re.fullmatch(r"func\.call @([A-Za-z0-9_.$-]+)\((.*)\) : .*", line)
        if match:
            function_match = re.search(
                r"func\.func @([A-Za-z0-9_.$-]+)", function[0]
            )
            invalid_call = (
                call_count
                or not function_match
                or match.group(1) != function_match.group(1)
            )
            if invalid_call:
                raise ValueError(
                    "solver root must call its lowered function exactly once"
                )
            call_args = [arg.strip() for arg in match.group(2).split(",")]
            arguments = [lookup(env, arg) for arg in call_args]
            assertions.extend(evaluate_function(function, arguments))
            call_count += 1
            continue
        if line == "smt.check sat {":
            check_count += 1
            continue
        if line in ("smt.yield", "}", "} unknown {", "} unsat {"):
            continue
        raise ValueError(f"unsupported solver-root operation: {line}")

    if call_count != 1 or check_count != 1:
        raise ValueError("solver root must contain one lowered call and one check")
    output.extend(assertions)
    output.append("(check-sat)")
    return "\n".join(output) + "\n"


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: lowered_mlir_to_smtlib.py <lowered-wrapper.mlir>")
    try:
        sys.stdout.write(translate(Path(sys.argv[1])))
    except (OSError, ValueError) as error:
        raise SystemExit(f"lowered_mlir_to_smtlib.py: {error}") from error
