# Issue #300 control-flow fusion reproduction

This evidence-only branch freezes production behavior at PR #484 head
`b7e9fae301040df771c669b7d7515fc0e5921b39`. It changes no production source, build input, or
toolchain input.

## Problem

Issue #300 accepts compute/constrain `scf.if` pairs when their conditions are provably identical
and when both operations yield the same number of results with the same types. PR #484 intentionally
implements a narrower first step: the condition must be the exact same SSA value and the constrain
conditional must have no results.

The two unsupported conditions are independently observable:

- `EquivalentConditionGap` uses two distinct `arith.cmpi eq` operations over the same operands.
- `ResultfulConstrainGap` gives both conditionals one externally used `!felt.type` result.
- `CombinedGap` exercises both conditions together.

`ExactSsaBaseline` proves that the supported exact-SSA, resultless case still fuses. Three negative
controls prove that different comparison predicates, different operands, and different result types
remain separate.

The analysis unit probe also records a complication for any future condition-equivalence fix:
`LightweightSignalEquivalenceAnalysis` currently treats `arith.cmpi eq` and `arith.cmpi ne` over the
same operands as equivalent because operation attributes are not part of its identity. Wiring that
analysis directly into fusion would therefore make the different-predicate control unsafe.

## Reproduction

Run the fork workflow `Reproduce issue 300 control-flow gaps` on branch
`repro/issue-300-control-flow-gaps`. It performs an exact Nix build and full test run, transforms the
seven split modules, and then enforces all of these conditions:

1. Production and build inputs are byte-identical to the frozen PR head, and the complete branch
   delta matches the committed evidence-only path allowlist.
2. The analysis probe runs and passes in the 1,327-test CTest suite.
3. The complete transformed output exactly matches the freshly generated FileCheck block in
   `current-output.full-check.llzk` after normalizing the generator's extra terminal blank line.
4. The supported baseline has one fused conditional with both branch constraints and its result use
   preserved.
5. Each issue-defined gap independently fails its intended fused-output check.
6. All three rejection controls retain their distinct compute and constrain conditionals.
7. Compiler stderr is empty, the full raw output is nonempty, and all revisions and tool versions
   are captured in the uploaded evidence artifact.

## Observed and expected behavior

Observed at the frozen head: only `ExactSsaBaseline` fuses. The equivalent-condition, resultful, and
combined cases retain separate compute and constrain conditionals.

Expected by issue #300: those three positive cases are fusion candidates, while the predicate,
operand, and result-type controls remain separate. This reproduction does not prescribe a broader
fusion implementation or result-remapping design; it proves only the accepted eligibility gaps and
the unsafe predicate-blind analysis behavior.
