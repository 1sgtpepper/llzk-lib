#!/usr/bin/env bash
set -euo pipefail

readonly MAIN_COMMIT=d1198631dbe9906cb149d1ddcd0dd1ca071dae6b
readonly RELEASE_COMMIT=b1b8d52ca4e6114cdd9a80417f96342a9f9e8b6c
readonly BN254_PRIME=21888242871839275222246405745257275088548364400416034343698204186575808495617
readonly PTAU_URL=https://storage.googleapis.com/zkevm/ptau/powersOfTau28_hez_final_08.ptau
readonly PTAU_SHA512=d6a8fb3a04feb600096c3b791f936a578c4e664d262e4aa24beed1b7a9a96aa5eb72864d628db247e9293384b74b36ffb52ca8d148d6e1b8b51e279fdf57b583
readonly FIXTURES=$GITHUB_WORKSPACE/validation/array-column-offset-proof
readonly WORK=$RUNNER_TEMP/llzk-array-column-offset-proof
readonly SOURCES=$WORK/sources
readonly ARTIFACTS=$FIXTURES/artifacts

rm -rf "$WORK" "$ARTIFACTS"
mkdir -p "$SOURCES/main" "$SOURCES/release" "$WORK/main" "$WORK/release" \
  "$ARTIFACTS/main" "$ARTIFACTS/release"

git -C "$GITHUB_WORKSPACE" rev-parse "$MAIN_COMMIT^{commit}"
git -C "$GITHUB_WORKSPACE" archive "$MAIN_COMMIT" | tar -x -C "$SOURCES/main"
(cd "$SOURCES/main" && nix --print-build-logs build '.#debugGCC')
MAIN_BIN=$(readlink -f "$SOURCES/main/result/bin")

"$MAIN_BIN/llzk-opt" --verify-each -llzk-array-to-scalar \
  "$FIXTURES/candidate.llzk" -o "$WORK/main/candidate.scalar.mlir"
"$MAIN_BIN/llzk-opt" --verify-each -llzk-array-to-scalar \
  "$FIXTURES/control.llzk" -o "$WORK/main/control.scalar.mlir"
cmp "$WORK/main/candidate.scalar.mlir" "$WORK/main/control.scalar.mlir"

"$MAIN_BIN/llzk-opt" --verify-each -llzk-full-r1cs-lowering \
  "$FIXTURES/candidate.llzk" -o "$WORK/main/candidate.r1cs.mlir"
"$MAIN_BIN/llzk-opt" --verify-each -llzk-full-r1cs-lowering \
  "$FIXTURES/control.llzk" -o "$WORK/main/control.r1cs.mlir"
cmp "$WORK/main/candidate.r1cs.mlir" "$WORK/main/control.r1cs.mlir"
"$MAIN_BIN/llzk-translate" --r1cs-to-binary --r1cs-prime="$BN254_PRIME" \
  "$WORK/main/candidate.r1cs.mlir" -o "$WORK/main/candidate.r1cs"
"$MAIN_BIN/llzk-translate" --r1cs-to-binary --r1cs-prime="$BN254_PRIME" \
  "$WORK/main/control.r1cs.mlir" -o "$WORK/main/control.r1cs"
cmp "$WORK/main/candidate.r1cs" "$WORK/main/control.r1cs"

"$MAIN_BIN/llzk-witgen" "$WORK/main/candidate.scalar.mlir" \
  --inputs "$FIXTURES/inputs.json" --output-scope=full-witness \
  --output-wtns "$WORK/main/candidate.wtns" > "$WORK/main/candidate.witness.json"
"$MAIN_BIN/llzk-witgen" "$WORK/main/control.scalar.mlir" \
  --inputs "$FIXTURES/inputs.json" --output-scope=full-witness \
  --output-wtns "$WORK/main/control.wtns" > "$WORK/main/control.witness.json"
cmp "$WORK/main/candidate.witness.json" "$WORK/main/control.witness.json"
cmp "$WORK/main/candidate.wtns" "$WORK/main/control.wtns"

git -C "$GITHUB_WORKSPACE" rev-parse "$RELEASE_COMMIT^{commit}"
git -C "$GITHUB_WORKSPACE" archive "$RELEASE_COMMIT" | tar -x -C "$SOURCES/release"
(cd "$SOURCES/release" && nix --print-build-logs build '.#debugGCC')
RELEASE_BIN=$(readlink -f "$SOURCES/release/result/bin")

"$RELEASE_BIN/llzk-opt" --verify-each -llzk-array-to-scalar \
  "$FIXTURES/candidate.llzk" -o "$WORK/release/candidate.scalar.mlir"
"$RELEASE_BIN/llzk-opt" --verify-each -llzk-array-to-scalar \
  "$FIXTURES/control.llzk" -o "$WORK/release/control.scalar.mlir"
cmp "$WORK/release/candidate.scalar.mlir" "$WORK/release/control.scalar.mlir"
"$RELEASE_BIN/llzk-opt" --verify-each -llzk-full-r1cs-lowering \
  "$WORK/release/candidate.scalar.mlir" -o "$WORK/release/candidate.r1cs.mlir"
"$RELEASE_BIN/llzk-opt" --verify-each -llzk-full-r1cs-lowering \
  "$WORK/release/control.scalar.mlir" -o "$WORK/release/control.r1cs.mlir"
cmp "$WORK/release/candidate.r1cs.mlir" "$WORK/release/control.r1cs.mlir"

# v2.1.2 has no R1CS binary exporter or WTNS writer. The exact release R1CS IR
# is passed to the exact-main serializer only after byte comparison proves the
# candidate and control relation, and the release witness JSON, match the
# exact-main outputs.
"$RELEASE_BIN/llzk-witgen" "$WORK/release/candidate.scalar.mlir" \
  --inputs "$FIXTURES/inputs.json" --output-scope=full-witness \
  > "$WORK/release/candidate.witness.json"
python3 - "$WORK/main/candidate.witness.json" "$WORK/release/candidate.witness.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as main_file:
    main = json.load(main_file)
with open(sys.argv[2], encoding="utf-8") as release_file:
    release = json.load(release_file)
if main != release:
    raise SystemExit("main and release full-witness JSON differ")
PY

"$MAIN_BIN/llzk-translate" --r1cs-to-binary --r1cs-prime="$BN254_PRIME" \
  "$WORK/release/candidate.r1cs.mlir" -o "$WORK/release/candidate.r1cs"
"$MAIN_BIN/llzk-translate" --r1cs-to-binary --r1cs-prime="$BN254_PRIME" \
  "$WORK/release/control.r1cs.mlir" -o "$WORK/release/control.r1cs"
cmp "$WORK/release/candidate.r1cs" "$WORK/release/control.r1cs"
cmp "$WORK/main/candidate.r1cs" "$WORK/release/candidate.r1cs"

npm install --prefix "$WORK/npm" --no-save snarkjs@0.7.5
SNARKJS="$WORK/npm/node_modules/.bin/snarkjs"
curl --fail --location --retry 3 "$PTAU_URL" -o "$WORK/powersoftau.ptau"
actual_ptau_sha512=$(sha512sum "$WORK/powersoftau.ptau" | cut -d ' ' -f1)
if [[ "$actual_ptau_sha512" != "$PTAU_SHA512" ]]; then
  echo "unexpected Powers of Tau checksum: $actual_ptau_sha512" >&2
  exit 1
fi
"$SNARKJS" powersoftau verify "$WORK/powersoftau.ptau"

for revision in main release; do
  "$SNARKJS" wtns check "$WORK/$revision/candidate.r1cs" "$WORK/main/candidate.wtns"
  "$SNARKJS" wtns check "$WORK/$revision/control.r1cs" "$WORK/main/control.wtns"
  "$SNARKJS" plonk setup "$WORK/$revision/candidate.r1cs" "$WORK/powersoftau.ptau" \
    "$WORK/$revision/circuit_final.zkey"
  "$SNARKJS" zkey export verificationkey "$WORK/$revision/circuit_final.zkey" \
    "$WORK/$revision/verification_key.json"
  "$SNARKJS" plonk prove "$WORK/$revision/circuit_final.zkey" \
    "$WORK/main/candidate.wtns" "$WORK/$revision/candidate.proof.json" \
    "$WORK/$revision/candidate.public.json"
  "$SNARKJS" plonk verify "$WORK/$revision/verification_key.json" \
    "$WORK/$revision/candidate.public.json" "$WORK/$revision/candidate.proof.json"
done

cp "$FIXTURES/candidate.llzk" "$FIXTURES/control.llzk" "$FIXTURES/inputs.json" \
  "$FIXTURES/source-oracle.txt" "$ARTIFACTS/"
cp "$WORK/main/candidate.scalar.mlir" "$WORK/main/control.scalar.mlir" \
  "$WORK/main/candidate.r1cs.mlir" "$WORK/main/control.r1cs.mlir" \
  "$WORK/main/candidate.r1cs" "$WORK/main/control.r1cs" \
  "$WORK/main/candidate.witness.json" "$WORK/main/control.witness.json" \
  "$WORK/main/candidate.wtns" "$WORK/main/control.wtns" \
  "$ARTIFACTS/main/"
cp "$WORK/main/circuit_final.zkey" "$WORK/main/verification_key.json" \
  "$WORK/main/candidate.proof.json" "$WORK/main/candidate.public.json" "$ARTIFACTS/main/"
cp "$WORK/release/candidate.scalar.mlir" "$WORK/release/control.scalar.mlir" \
  "$WORK/release/candidate.r1cs.mlir" "$WORK/release/control.r1cs.mlir" \
  "$WORK/release/candidate.r1cs" "$WORK/release/control.r1cs" \
  "$WORK/release/candidate.witness.json" "$WORK/release/circuit_final.zkey" \
  "$WORK/release/candidate.proof.json" "$WORK/release/candidate.public.json" \
  "$WORK/release/verification_key.json" "$ARTIFACTS/release/"

cat > "$ARTIFACTS/validation-summary.txt" <<EOF
main revision: $MAIN_COMMIT
release revision: $RELEASE_COMMIT
Powers of Tau URL: $PTAU_URL
Powers of Tau SHA-512: $actual_ptau_sha512
candidate/control ArrayToScalar output: byte-identical at both revisions
candidate/control R1CS IR and binary: byte-identical at both revisions
main/release candidate R1CS binary: byte-identical after exact-main serialization
main/release witness JSON: semantically identical
PLONK setup, witness check, proof, and verification: passed independently for both revisions
release caveat: v2.1.2 ships no R1CS binary exporter or WTNS writer; exact release R1CS IR and witness JSON were bridged through the exact-main serializer only after the byte/semantic equality checks above
EOF
(cd "$ARTIFACTS" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum) \
  > "$ARTIFACTS/SHA256SUMS"
