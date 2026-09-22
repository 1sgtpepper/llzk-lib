#!/usr/bin/env bash
set -euo pipefail

tool_root=${1:?usage: run.sh <llzk-result> <output-directory>}
output_dir=${2:?usage: run.sh <llzk-result> <output-directory>}
asset_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

mkdir -p "$output_dir"

opt="$tool_root/bin/llzk-opt"
translate="$tool_root/bin/llzk-translate"
check="$tool_root/bin/llzk-smt-check"
solver=${Z3_BIN:-z3}

for tool in "$opt" "$translate" "$check"; do
  test -x "$tool"
done

"$opt" \
  --llzk-to-smt-no-cf-naive='field=babybear' \
  --llzk-to-smt-cf-only \
  "$asset_dir/reproducer.llzk" > "$output_dir/candidate-naive.mlir"
"$opt" \
  --llzk-to-smt-no-cf='field=babybear' \
  --llzk-to-smt-cf-only \
  "$asset_dir/reproducer.llzk" > "$output_dir/candidate-optimized.mlir"
"$opt" \
  --llzk-to-smt-no-cf-naive='field=babybear' \
  --llzk-to-smt-cf-only \
  "$asset_dir/control.llzk" > "$output_dir/control-naive.mlir"

python3 "$asset_dir/assert_ir.py" \
  "$output_dir/candidate-naive.mlir" \
  "$output_dir/candidate-optimized.mlir" \
  "$output_dir/control-naive.mlir"

python3 "$asset_dir/wrap_solver.py" \
  "$output_dir/candidate-naive.mlir" CmpBoundary unsat \
  > "$output_dir/candidate-naive-wrapper.mlir"
python3 "$asset_dir/wrap_solver.py" \
  "$output_dir/candidate-optimized.mlir" CmpBoundary sat \
  > "$output_dir/candidate-optimized-wrapper.mlir"
python3 "$asset_dir/wrap_solver.py" \
  "$output_dir/control-naive.mlir" CmpBoundaryControl sat \
  > "$output_dir/control-naive-wrapper.mlir"

for name in candidate-naive candidate-optimized control-naive; do
  "$translate" --smt-to-smtlib "$output_dir/$name-wrapper.mlir" \
    > "$output_dir/$name.smt2"
  "$check" "$output_dir/$name.smt2" --solver-binary="$solver" \
    | tee "$output_dir/$name.solver.txt"
done

printf '%s\n' \
  'SMT comparison validation passed:' \
  '  candidate / naive lowering: unsat (wrong relation)' \
  '  candidate / optimized lowering: sat (control relation)' \
  '  no-wrap control / naive lowering: sat'
