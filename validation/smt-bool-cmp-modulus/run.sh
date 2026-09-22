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

test -x "$opt"

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
"$opt" \
  --llzk-to-smt-no-cf='field=babybear' \
  --llzk-to-smt-cf-only \
  "$asset_dir/control.llzk" > "$output_dir/control-optimized.mlir"
"$opt" \
  --llzk-to-smt-no-cf-naive='field=babybear' \
  --llzk-to-smt-cf-only \
  "$asset_dir/explicit-mod-control.llzk" > "$output_dir/explicit-mod-naive.mlir"
"$opt" \
  --llzk-to-smt-no-cf='field=babybear' \
  --llzk-to-smt-cf-only \
  "$asset_dir/explicit-mod-control.llzk" > "$output_dir/explicit-mod-optimized.mlir"

python3 "$asset_dir/assert_ir.py" \
  "$output_dir/candidate-naive.mlir" \
  "$output_dir/candidate-optimized.mlir" \
  "$output_dir/control-naive.mlir" \
  "$output_dir/control-optimized.mlir" \
  "$output_dir/explicit-mod-naive.mlir" \
  "$output_dir/explicit-mod-optimized.mlir"

if [[ ! -x "$translate" || ! -x "$check" ]]; then
  translate_status=unavailable
  check_status=unavailable
  if [[ -x "$translate" ]]; then translate_status=available; fi
  if [[ -x "$check" ]]; then check_status=available; fi
  printf '%s\n' \
    "Exact target provides llzk-opt but lacks the downstream SMT-LIB tools:" \
    "  llzk-translate: $translate_status" \
    "  llzk-smt-check: $check_status" \
    "Exact lowered MLIR is available above; SMT-LIB translation and solver validation were not run." \
    > "$output_dir/downstream-validation-unavailable.txt"
  printf '%s\n' \
    'PARTIAL: exact target lacks llzk-translate and/or llzk-smt-check; lowered MLIR artifacts were retained.'
  exit 0
fi

python3 "$asset_dir/wrap_solver.py" \
  "$output_dir/candidate-naive.mlir" smt_CmpBoundary sat 2013265920 1 \
  > "$output_dir/candidate-naive-wrapper.mlir"
python3 "$asset_dir/wrap_solver.py" \
  "$output_dir/candidate-optimized.mlir" smt_CmpBoundary sat 2013265920 1 \
  > "$output_dir/candidate-optimized-wrapper.mlir"
python3 "$asset_dir/wrap_solver.py" \
  "$output_dir/control-naive.mlir" smt_CmpBoundaryControl sat 1 1 \
  > "$output_dir/control-naive-wrapper.mlir"
python3 "$asset_dir/wrap_solver.py" \
  "$output_dir/control-optimized.mlir" smt_CmpBoundaryControl sat 1 1 \
  > "$output_dir/control-optimized-wrapper.mlir"
python3 "$asset_dir/wrap_solver.py" \
  "$output_dir/explicit-mod-naive.mlir" smt_CmpBoundaryExplicitMod unsat 2013265920 1 \
  > "$output_dir/explicit-mod-naive-wrapper.mlir"
python3 "$asset_dir/wrap_solver.py" \
  "$output_dir/explicit-mod-optimized.mlir" smt_CmpBoundaryExplicitMod unsat 2013265920 1 \
  > "$output_dir/explicit-mod-optimized-wrapper.mlir"

for name in \
  candidate-naive \
  candidate-optimized \
  control-naive \
  control-optimized \
  explicit-mod-naive \
  explicit-mod-optimized; do
  "$translate" --smt-to-smtlib "$output_dir/$name-wrapper.mlir" \
    > "$output_dir/$name.smt2"
  "$check" "$output_dir/$name.smt2" --solver-binary="$solver" \
    | tee "$output_dir/$name.solver.txt"
done

printf '%s\n' \
  'SMT comparison validation passed:' \
  '  candidate / both lowerings: sat (wrongly accepts field-zero inequality)' \
  '  no-wrap control / both lowerings: sat' \
  '  explicit-mod control / both lowerings: unsat'
