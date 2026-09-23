#!/usr/bin/env bash
set -euo pipefail

readonly ORIGINAL_MAIN=d1198631dbe9906cb149d1ddcd0dd1ca071dae6b
readonly CURRENT_MAIN=0b0f78af688af659ad09f15631199a998f859ce2
readonly RELEASE=b1b8d52ca4e6114cdd9a80417f96342a9f9e8b6c
readonly BABYBEAR_PRIME=2013265921
readonly UPSTREAM=https://github.com/project-llzk/llzk-lib.git
readonly FIXTURES=$GITHUB_WORKSPACE/validation/field-wrap-while-to-for
readonly WORK=$RUNNER_TEMP/llzk-field-wrap-while-to-for
readonly SOURCES=$WORK/sources
readonly ARTIFACTS=$FIXTURES/artifacts

rm -rf "$WORK" "$ARTIFACTS"
mkdir -p "$SOURCES" "$WORK" "$ARTIFACTS"
cp "$FIXTURES/candidate.llzk" "$FIXTURES/no-wrap-control.llzk" \
  "$FIXTURES/source-reference.llzk" "$FIXTURES/inputs.json" \
  "$FIXTURES/source-oracle.txt" "$ARTIFACTS/"

git -C "$GITHUB_WORKSPACE" fetch --no-tags "$UPSTREAM" \
  main:refs/remotes/validation/upstream-main
git -C "$GITHUB_WORKSPACE" fetch --no-tags "$UPSTREAM" \
  refs/tags/v2.1.2:refs/tags/v2.1.2
fetched_main=$(git -C "$GITHUB_WORKSPACE" rev-parse \
  "refs/remotes/validation/upstream-main^{commit}")
if [[ "$fetched_main" != "$CURRENT_MAIN" ]]; then
  echo "upstream main moved: expected $CURRENT_MAIN, fetched $fetched_main" >&2
  exit 1
fi
for revision in "$ORIGINAL_MAIN" "$CURRENT_MAIN" "$RELEASE"; do
  git -C "$GITHUB_WORKSPACE" cat-file -e "$revision^{commit}"
done
if [[ $(git -C "$GITHUB_WORKSPACE" rev-parse "v2.1.2^{commit}") != "$RELEASE" ]]; then
  echo "the v2.1.2 tag no longer resolves to the pinned release commit" >&2
  exit 1
fi

source_revision() {
  local name=$1
  local revision=$2
  local mode=$3
  local source="$SOURCES/$name"
  local work="$WORK/$name"
  local output="$ARTIFACTS/$name"
  mkdir -p "$source" "$work" "$output"
  git -C "$GITHUB_WORKSPACE" archive "$revision" | tar -x -C "$source"
  (
    cd "$source"
    nix --print-build-logs build '.#debugGCC'
  )
  local bin
  bin=$(readlink -f "$source/result/bin")
  printf '%s\n' "$bin" > "$output/toolchain-bin.txt"
  printf '%s\n' "$revision" > "$output/source-revision.txt"

  for scenario in candidate no-wrap-control source-reference; do
    local fixture="$FIXTURES/$scenario.llzk"
    "$bin/llzk-opt" --verify-each --llzk-while-to-for --canonicalize \
      --llzk-flatten --canonicalize "$fixture" -o "$work/$scenario.prepared.mlir"
    cp "$work/$scenario.prepared.mlir" "$output/$scenario.prepared.mlir"
    if [[ "$mode" == "main" ]]; then
      "$bin/llzk-opt" --verify-each --llzk-full-r1cs-lowering \
        "$fixture" -o "$work/$scenario.r1cs.mlir"
    else
      # v2.1.2 exposes WhileToFor and flattening as public passes, but its
      # full-R1CS pipeline consumes already-flattened constraints and does not
      # schedule WhileToFor itself. Apply those public release passes first.
      "$bin/llzk-opt" --verify-each=0 --llzk-full-r1cs-lowering \
        "$work/$scenario.prepared.mlir" -o "$work/$scenario.r1cs.mlir"
    fi
    cp "$work/$scenario.r1cs.mlir" "$output/$scenario.r1cs.mlir"
  done

  if [[ "$mode" == "main" ]]; then
    for scenario in candidate no-wrap-control source-reference; do
      "$bin/llzk-translate" --r1cs-to-binary --r1cs-prime="$BABYBEAR_PRIME" \
        "$WORK/$name/$scenario.r1cs.mlir" -o "$WORK/$name/$scenario.r1cs"
      cp "$WORK/$name/$scenario.r1cs" "$ARTIFACTS/$name/"
    done
    for scenario in candidate no-wrap-control; do
      "$bin/llzk-witgen" "$FIXTURES/$scenario.llzk" --inputs "$FIXTURES/inputs.json" \
        --output-scope=full-witness --output-wtns "$WORK/$name/$scenario.wtns" \
        > "$WORK/$name/$scenario.witness.json"
      cp "$WORK/$name/$scenario.witness.json" "$WORK/$name/$scenario.wtns" \
        "$ARTIFACTS/$name/"
    done
  else
    for scenario in candidate no-wrap-control source-reference; do
      normalize_release_r1cs_header "$WORK/$name/$scenario.r1cs.mlir" \
        "$WORK/$name/$scenario.r1cs.bridge.mlir"
      "$CURRENT_BIN/llzk-opt" --verify-each "$WORK/$name/$scenario.r1cs.bridge.mlir" \
        -o /dev/null
      "$CURRENT_BIN/llzk-translate" --r1cs-to-binary --r1cs-prime="$BABYBEAR_PRIME" \
        "$WORK/$name/$scenario.r1cs.bridge.mlir" -o "$WORK/$name/$scenario.r1cs"
      cp "$WORK/$name/$scenario.r1cs.bridge.mlir" "$WORK/$name/$scenario.r1cs" \
        "$ARTIFACTS/$name/"
    done
    for scenario in candidate no-wrap-control; do
      "$bin/llzk-witgen" "$FIXTURES/$scenario.llzk" --inputs "$FIXTURES/inputs.json" \
        --output-scope=full-witness > "$WORK/$name/$scenario.witness.json"
      cp "$WORK/$name/$scenario.witness.json" "$ARTIFACTS/$name/"
    done
  fi

}

normalize_release_r1cs_header() {
  python3 - "$1" "$2" <<'PY'
import re
import sys

source, destination = sys.argv[1:]
text = open(source, encoding="utf-8").read()
stale_main = ", llzk.main = !struct.type<@Main>"
if text.count(stale_main) != 1:
    raise SystemExit(f"{source}: expected one stale llzk.main reference")
if 'llzk.lang = "r1cs"' not in text or text.count('"r1cs.circuit"') != 1:
    raise SystemExit(f"{source}: not the expected lowered R1CS module")
label_pattern = re.compile(r'("r1cs[.]def"[(][)] [{]label = )([0-9]+)( : ui32)')
labels = [int(match.group(2)) for match in label_pattern.finditer(text)]
if labels != list(range(len(labels))):
    raise SystemExit(f"{source}: expected unique zero-based release labels, got {labels}")
if not labels or labels[-1] == (1 << 32) - 1:
    raise SystemExit(f"{source}: release labels cannot be remapped to nonzero u32 values")
normalized = label_pattern.sub(
    lambda match: f"{match.group(1)}{int(match.group(2)) + 1}{match.group(3)}", text
)
normalized = normalized.replace(stale_main, "", 1)
if "llzk.main" in normalized:
    raise SystemExit(f"{source}: unexpected remaining llzk.main metadata")
open(destination, "w", encoding="utf-8").write(normalized)
PY
}

assert_source_oracle() {
  python3 - <<'PY'
P = 2013265921

def body_values(lower, upper, step):
    values = []
    value = lower
    for _ in range(8):
        if value >= upper:
            return values
        values.append(value)
        value = (value + step) % P
    raise SystemExit("source loop did not terminate within the bounded oracle")

wrapped = body_values(1, P - 2, P - 1)
control = body_values(1, 2, 1)
if wrapped != [1, 0] or control != [1]:
    raise SystemExit(f"unexpected field loop traces: wrapped={wrapped}, control={control}")
if 1 in wrapped and 0 in wrapped:
    print(f"source oracle: wrapped loop body values={wrapped}; out=1 fails the second equality")
print(f"source oracle: no-wrap body values={control}; out=1 satisfies the relation")
PY
}

source_revision original-main "$ORIGINAL_MAIN" main
source_revision current-main "$CURRENT_MAIN" main
CURRENT_BIN=$(cat "$ARTIFACTS/current-main/toolchain-bin.txt")
readonly CURRENT_BIN
ORIGINAL_BIN=$(cat "$ARTIFACTS/original-main/toolchain-bin.txt")
readonly ORIGINAL_BIN
source_revision release-v2.1.2 "$RELEASE" release

for name in original-main current-main release-v2.1.2; do
  cmp "$WORK/$name/candidate.r1cs" "$WORK/$name/no-wrap-control.r1cs"
done

"$ORIGINAL_BIN/llzk-witgen" "$FIXTURES/candidate.llzk" --inputs "$FIXTURES/inputs.json" \
  --output-scope=full-witness --output-wtns "$WORK/original-main/candidate.wtns" \
  > "$WORK/original-main/candidate.witness.json"
"$ORIGINAL_BIN/llzk-witgen" "$FIXTURES/no-wrap-control.llzk" --inputs "$FIXTURES/inputs.json" \
  --output-scope=full-witness --output-wtns "$WORK/original-main/no-wrap-control.wtns" \
  > "$WORK/original-main/no-wrap-control.witness.json"
cp "$WORK/original-main/candidate.wtns" "$WORK/original-main/candidate.witness.json" \
  "$WORK/original-main/no-wrap-control.wtns" "$WORK/original-main/no-wrap-control.witness.json" \
  "$ARTIFACTS/original-main/"
cmp "$WORK/original-main/candidate.wtns" "$WORK/original-main/no-wrap-control.wtns"
cmp "$WORK/original-main/candidate.witness.json" "$WORK/original-main/no-wrap-control.witness.json"

"$CURRENT_BIN/llzk-witgen" "$FIXTURES/candidate.llzk" --inputs "$FIXTURES/inputs.json" \
  --output-scope=full-witness > "$WORK/current-main/candidate.witness.json"
"$CURRENT_BIN/llzk-witgen" "$FIXTURES/no-wrap-control.llzk" --inputs "$FIXTURES/inputs.json" \
  --output-scope=full-witness > "$WORK/current-main/no-wrap-control.witness.json"
cp "$WORK/current-main/candidate.witness.json" "$WORK/current-main/no-wrap-control.witness.json" \
  "$ARTIFACTS/current-main/"
cmp "$WORK/current-main/candidate.witness.json" "$WORK/current-main/no-wrap-control.witness.json"

for name in current-main release-v2.1.2; do
  python3 - "$WORK/original-main/candidate.witness.json" \
    "$WORK/$name/candidate.witness.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    expected = json.load(source)
with open(sys.argv[2], encoding="utf-8") as target:
    actual = json.load(target)
if actual != expected:
    raise SystemExit(f"witness JSON differs: {sys.argv[2]}")
PY
done

cmp "$WORK/release-v2.1.2/candidate.witness.json" \
  "$WORK/release-v2.1.2/no-wrap-control.witness.json"

python3 "$FIXTURES/check_release_r1cs_ir.py" \
  --candidate "$WORK/release-v2.1.2/candidate.r1cs.mlir" \
  --control "$WORK/release-v2.1.2/no-wrap-control.r1cs.mlir" \
  --reference "$WORK/release-v2.1.2/source-reference.r1cs.mlir" \
  --witness "$WORK/release-v2.1.2/candidate.witness.json" \
  --prime "$BABYBEAR_PRIME" \
  | tee "$ARTIFACTS/release-v2.1.2/direct-r1cs-mlir-check.txt"

for name in original-main current-main release-v2.1.2; do
  python3 "$FIXTURES/check_r1cs_wtns.py" \
    --candidate "$WORK/$name/candidate.r1cs" \
    --control "$WORK/$name/no-wrap-control.r1cs" \
    --reference "$WORK/$name/source-reference.r1cs" \
    --witness "$WORK/original-main/candidate.wtns"
done

assert_source_oracle

cat > "$ARTIFACTS/validation-summary.txt" <<EOF
Original frozen main: $ORIGINAL_MAIN
Current upstream main: $CURRENT_MAIN
Published release v2.1.2: $RELEASE
Field: BabyBear ($BABYBEAR_PRIME)
Source loop trace: candidate [1, 0], no-wrap control [1]
Generated witness: public out=1; exact-main llzk-witgen wrote WTNS v2
R1CS check: candidate and no-wrap accept the witness; explicit source reference rejects it
Release path: exact release llzk-while-to-for, canonicalize, and llzk-flatten run before its already-flattened full-R1CS lowering; the exact release R1CS dialect output is evaluated directly
Release witness interpretation: v2.1.2 llzk-witgen JSON is checked against exact-release R1CS dialect MLIR by check_release_r1cs_ir.py; the main binary exporter is only a cross-check
Main witness interpretation: the emitted BabyBear R1CS/WTNS is checked by the standard R1CS equation evaluator in check_r1cs_wtns.py
Proof limitation: no cryptographic proof or shipped BabyBear prover is claimed
EOF
find "$ARTIFACTS" -type f ! -name SHA256SUMS -print0 | sort -z | \
  xargs -0 sha256sum > "$ARTIFACTS/SHA256SUMS"
