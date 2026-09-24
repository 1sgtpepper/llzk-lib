Fork-only CI validation of felt induction-variable wraparound during WhileToFor
conversion. CI preserves the exact output of the standalone conversion pass as
well as the fully lowered R1CS and witness artifacts. See source-oracle.txt for
the exact source trace and control behavior. The generic evaluator checks the
emitted standard BabyBear R1CS against an LLZK WTNS witness; it does not claim
a cryptographic proof or an LLZK BabyBear prover.
