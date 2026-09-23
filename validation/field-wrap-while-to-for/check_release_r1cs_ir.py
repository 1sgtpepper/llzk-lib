#!/usr/bin/env python3
"""Evaluate the pinned fixture's exact v2.1.2 R1CS dialect output."""

import argparse
import json
import re
import sys
from pathlib import Path


PRIME = 2013265921
LINE = tuple[int, dict[int, int]]


def check(path: Path, witness_path: Path) -> tuple[bool, int, int]:
    text = path.read_text(encoding="utf-8")
    marker = '"r1cs.circuit"() ({'
    end_marker = '  }) {arg_attrs = '
    if text.count(marker) != 1:
        raise ValueError(f"{path}: expected exactly one R1CS circuit")
    start = text.index(marker) + len(marker)
    end = text.find(end_marker, start)
    if end < 0:
        raise ValueError(f"{path}: malformed R1CS circuit body")

    signals: dict[str, tuple[int, bool]] = {}
    definitions: dict[int, bool] = {}
    values: dict[str, LINE] = {}
    constraints: list[tuple[LINE, LINE, LINE]] = []

    def add(lhs: LINE, rhs: LINE) -> LINE:
        terms = dict(lhs[1])
        for label, coefficient in rhs[1].items():
            terms[label] = (terms.get(label, 0) + coefficient) % PRIME
        return (lhs[0] + rhs[0]) % PRIME, {
            label: coefficient for label, coefficient in terms.items() if coefficient % PRIME
        }

    patterns = [
        ("def", re.compile(r'(%\d+) = "r1cs\.def"\(\) \{label = (\d+) : ui32(, pub = #r1cs\.pub)?\} : \(\) -> !r1cs\.signal')),
        ("const", re.compile(r'(%\d+) = "r1cs\.const"\(\) \{value = #r1cs<felt (-?\d+) : i64>\} : \(\) -> !r1cs\.linear')),
        ("to_linear", re.compile(r'(%\d+) = "r1cs\.to_linear"\((%\d+)\) : \(!r1cs\.signal\) -> !r1cs\.linear')),
        ("mul_const", re.compile(r'(%\d+) = "r1cs\.mul_const"\((%\d+)\) \{constValue = #r1cs<felt (-?\d+) : i64>\} : \(!r1cs\.linear\) -> !r1cs\.linear')),
        ("add", re.compile(r'(%\d+) = "r1cs\.add"\((%\d+), (%\d+)\) : \(!r1cs\.linear, !r1cs\.linear\) -> !r1cs\.linear')),
        ("constrain", re.compile(r'"r1cs\.constrain"\((%\d+), (%\d+), (%\d+)\) : \(!r1cs\.linear, !r1cs\.linear, !r1cs\.linear\) -> \(\)')),
    ]

    for line_number, raw in enumerate(text[start:end].splitlines(), start=2):
        line = raw.strip()
        if not line:
            continue
        for kind, pattern in patterns:
            match = pattern.fullmatch(line)
            if match is None:
                continue
            groups = match.groups()
            if kind == "def":
                value, raw_label, public_attr = groups
                label = int(raw_label)
                if label in definitions:
                    raise ValueError(f"{path}:{line_number}: duplicate signal label {label}")
                public = public_attr is not None
                definitions[label] = public
                signals[value] = (label, public)
            elif kind == "const":
                value, raw_constant = groups
                values[value] = (int(raw_constant) % PRIME, {})
            elif kind == "to_linear":
                value, signal = groups
                if signal not in signals:
                    raise ValueError(f"{path}:{line_number}: undefined signal {signal}")
                values[value] = (0, {signals[signal][0]: 1})
            elif kind == "mul_const":
                value, source, raw_coefficient = groups
                if source not in values:
                    raise ValueError(f"{path}:{line_number}: undefined linear value {source}")
                coefficient = int(raw_coefficient) % PRIME
                source_constant, source_terms = values[source]
                values[value] = (
                    source_constant * coefficient % PRIME,
                    {label: term * coefficient % PRIME for label, term in source_terms.items()},
                )
            elif kind == "add":
                value, lhs, rhs = groups
                if lhs not in values or rhs not in values:
                    raise ValueError(f"{path}:{line_number}: undefined add operand")
                values[value] = add(values[lhs], values[rhs])
            else:
                lhs, rhs, output = groups
                if any(name not in values for name in groups):
                    raise ValueError(f"{path}:{line_number}: undefined constraint operand")
                constraints.append((values[lhs], values[rhs], values[output]))
            break
        else:
            raise ValueError(f"{path}:{line_number}: unsupported R1CS operation: {line}")

    witness = json.loads(witness_path.read_text(encoding="utf-8"))
    outputs = witness.get("signals")
    if witness.get("inputs") != {} or not isinstance(outputs, dict) or len(outputs) != 1:
        raise ValueError(f"{witness_path}: expected no inputs and exactly one output signal")
    public_labels = [label for label, public in definitions.items() if public]
    if len(definitions) != 1 or len(public_labels) != 1:
        raise ValueError(f"{path}: expected exactly one public signal and no private signals")
    output = int(next(iter(outputs.values()))) % PRIME

    def evaluate(linear: LINE) -> int:
        result = linear[0]
        for label, coefficient in linear[1].items():
            if label != public_labels[0]:
                raise ValueError(f"{path}: unassigned witness signal label {label}")
            result += coefficient * output
        return result % PRIME

    failed = next(
        (
            index
            for index, (lhs, rhs, target) in enumerate(constraints, start=1)
            if evaluate(lhs) * evaluate(rhs) % PRIME != evaluate(target)
        ),
        None,
    )
    return failed is None, len(constraints), output


def main() -> int:
    parser = argparse.ArgumentParser()
    for name in ("candidate", "control", "reference", "witness"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--prime", type=int, required=True)
    args = parser.parse_args()
    if args.prime != PRIME:
        parser.error(f"--prime must equal the pinned BabyBear prime {PRIME}")

    failures = []
    for name, path, expected in (
        ("wrapped candidate", args.candidate, True),
        ("no-wrap control", args.control, True),
        ("source reference", args.reference, False),
    ):
        status, count, output = check(path, args.witness)
        result = "SAT" if status else "UNSAT"
        print(f"{name}: witness=out:{output}, constraints={count}, result={result}")
        if status is not expected:
            failures.append(f"{name}: expected {'SAT' if expected else 'UNSAT'}, got {result}")

    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("Exact v2.1.2 R1CS dialect accepts the candidate/control witness and rejects it for the source reference.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
