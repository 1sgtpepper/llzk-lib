#!/usr/bin/env python3
"""Add a pinned SMT solver root around one lowered product function."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("function")
    parser.add_argument("expected", choices=("sat", "unsat"))
    parser.add_argument("arg0", type=int)
    parser.add_argument("arg1", type=int)
    args = parser.parse_args()

    module = args.input.read_text(encoding="utf-8")
    function_marker = f"func.func @{args.function}("
    if function_marker not in module:
        raise SystemExit(f"missing lowered function @{args.function}")
    if "smt.solver" in module:
        raise SystemExit("lowered module already contains an SMT solver root")

    close = module.rfind("\n}")
    if close < 0:
        raise SystemExit("could not find the root module terminator")

    solver = f'''  smt.solver () : () -> () {{
    smt.set_info ":llzk-root" "{args.function}"
    smt.set_info ":llzk-stage" "field-comparison"
    smt.set_info ":status" {args.expected}
    %a = smt.declare_fun "a" : !smt.int
    %b = smt.declare_fun "b" : !smt.int
    %expected_a = smt.int.constant {args.arg0}
    %expected_b = smt.int.constant {args.arg1}
    %pin_a = smt.eq %a, %expected_a : !smt.int
    %pin_b = smt.eq %b, %expected_b : !smt.int
    smt.assert %pin_a
    smt.assert %pin_b
    func.call @{args.function}(%a, %b) : (!smt.int, !smt.int) -> ()
    smt.check sat {{
      smt.yield
    }} unknown {{
      smt.yield
    }} unsat {{
      smt.yield
    }}
    smt.yield
  }}
'''
    print(module[:close] + "\n" + solver + module[close:], end="")


if __name__ == "__main__":
    main()
