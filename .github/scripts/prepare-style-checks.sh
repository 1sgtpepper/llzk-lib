#!/usr/bin/env bash
# Configure the checked-out revision and generate the headers needed by Clang-tidy.
set -eo pipefail
source "$stdenv/setup"
export CXXFLAGS="${NIX_CFLAGS_COMPILE:-}"
runPhase configurePhase
test -s compile_commands.json
ninja -t targets all > "$STYLE_OUTPUT/ninja-targets.txt"
mapfile -t generated_targets < <(sed -n 's/^\([^ /:]*IncGen\): phony$/\1/p' "$STYLE_OUTPUT/ninja-targets.txt" | sort -u)
test "${#generated_targets[@]}" -gt 0
printf '%s\n' "${generated_targets[@]}" > "$STYLE_OUTPUT/generated-targets.txt"
ninja -j2 "${generated_targets[@]}"
python3 "$STYLE_CONTROL/.github/scripts/check-pr-style.py" "$STYLE_SOURCE" "$PWD" "$STYLE_OUTPUT" "$STYLE_CONTROL/.github/style-targets.json" "$STYLE_LABEL"
