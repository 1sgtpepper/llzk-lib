#!/usr/bin/env python3
"""Prove two SMT-LIB assertion sets equivalent by asking Z3 for a witness."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def read_script(path: Path) -> tuple[list[str], list[str]]:
    declarations: list[str] = []
    assertions: list[str] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line.startswith("(declare-fun "):
            declarations.append(line)
        elif line.startswith("(assert ") and line.endswith(")"):
            assertions.append(line[len("(assert ") : -1])
        elif line and not (
            line == "(set-logic ALL)"
            or line == "(check-sat)"
            or line.startswith("(set-info ")
        ):
            raise ValueError(f"{path}: unsupported SMT-LIB command {line}")
    if not assertions:
        raise ValueError(f"{path}: no assertions found")
    return declarations, assertions


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    parser.add_argument("--solver-binary", default="z3")
    args = parser.parse_args()

    left_declarations, left_assertions = read_script(args.left)
    right_declarations, right_assertions = read_script(args.right)
    if set(left_declarations) != set(right_declarations):
        raise ValueError("SMT-LIB scripts declare different symbols")
    declarations = list(dict.fromkeys(left_declarations + right_declarations))
    left = "(and " + " ".join(left_assertions) + ")"
    right = "(and " + " ".join(right_assertions) + ")"
    query = "\n".join(
        [
            "(set-logic ALL)",
            *declarations,
            f"(assert (xor {left} {right}))",
            "(check-sat)",
            "",
        ]
    )
    result = subprocess.run(
        [args.solver_binary, "-in", "-smt2"],
        input=query,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise SystemExit(f"Z3 failed while comparing SMT-LIB:\n{result.stderr}")
    status = result.stdout.strip()
    if status != "unsat":
        raise SystemExit(f"SMT-LIB assertion sets differ; Z3 returned {status!r}")
    print("equivalent: xor is unsat")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit(f"compare_smtlib.py: {error}") from error
