//===-- LLZKTransformationPasses.td ------------------------*- tablegen -*-===//
//
// Part of the LLZK Project, under the Apache License v2.0.
// See LICENSE.txt for license information.
// Copyright 2025 Veridise Inc.
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//

#ifndef LLZK_TRANSFORMATION_PASSES_TD
#define LLZK_TRANSFORMATION_PASSES_TD

include "llzk/Pass/PassBase.td"

def RedundantReadAndWriteEliminationPass
    : LLZKPass<"llzk-duplicate-read-write-elim"> {
  let summary = "Remove redundant reads and writes";
  let description = [{
    Remove redundant or unnecessary reads and writes to struct members, arrays,
    globals, and RAM. Global writes are removed when they are overwritten or
    write an already-known value. RAM stores require the same translated address
    SSA value.

    Struct-member accesses match only when the base, member, table offset,
    affine map metadata, and translated affine operands match. Omitted
    member-read offsets and member writes denote the current row.

    Stateful reads are reused when the current analysis state proves the read
    observes the same value. Writes and stores update that known state, while
    unknown or non-read state effects invalidate it.
  }];
}

def RedundantOperationEliminationPass : LLZKPass<"llzk-duplicate-op-elim"> {
  let summary = "Remove redundant operations";
  let description = [{
    Remove equivalent memory-effect-free operations and duplicate LLZK constraints.

    Pass should be run after `llzk-duplicate-read-write-elim` for maximum effect.
  }];
}

def UnusedDeclarationEliminationPass
    : LLZKPass<"llzk-unused-declaration-elim"> {
  let summary = "Remove unused member and struct declarations";
  let description = [{
    Remove member and struct declarations that are unused within the current compilation
    unit. Note that this pass may cause linking issues with external modules that
    depend on any unused member and struct declarations from this compilation unit.

    Pass should be run after `llzk-duplicate-read-write-elim`
    and `llzk-duplicate-op-elim` for maximum effect.
  }];
  let options = [Option<"removeStructs", "remove-structs", "bool",
                        /* default */ "false",
                        "Whether to remove unused struct definitions as well. "
                        "Requires module to declare a Main component, "
                        "otherwise all components will appear unused.">,
  ];
}

def RemoveUnusedDiscardableAllocationsPass
    : LLZKPass<"llzk-remove-unused-discardable-allocations"> {
  let summary = "Remove unread discardable allocations and their dead stores";
  let description = [{
    Remove ops with the selected allocator operation name when they are marked with
    `MemAlloc<DiscardableAllocationResource>`, the allocation has no reads, and every direct user is
    a discardable allocation accessor that can be erased as a dead store.
  }];
  let options = [Option<
      "allocatorOpName", "allocator-op", "std::string",
      /* default */ "\"\"",
      "Operation name of the discardable allocator to remove">];
}

def PolyLoweringPass : LLZKPass<"llzk-poly-lowering-pass"> {
  let summary =
      "Lower the degree of all polynomial equations to a specified maximum";
  let description = [{
    Rewrites constraint expressions into an (observationally) equivalent system where the degree of
    every polynomial is less than or equal to the specified maximum.

    High-degree subexpressions are factored into auxiliary struct members. The pass also recurses
    through degree-neutral roots such as `felt.add`, `felt.sub`, and `felt.neg`, so composite
    equality operands and struct constrain call arguments satisfy their degree bounds.

    The pass expects already-flattened, straight-line `compute` and `constrain` bodies. Run
    `llzk-flatten` or another control-flow lowering pass before this pass; nested-region,
    successor-bearing, or multi-block functions are rejected instead of being rewritten with
    component-lifetime auxiliary members.

    This pass is best used on already-flattened input as part of the `-llzk-full-poly-lowering`
    pipeline, which includes additional cleanup passes to ensure correctness and optimal
    performance.
  }];
  let options = [Option<"maxDegree", "max-degree", "unsigned",
                        /* default */ "2",
                        "Maximum degree of constraint polynomials "
                        "(default 2, minimum 2)">,
  ];
}

def ComputeConstrainToProductPass
    : LLZKPass<"llzk-compute-constrain-to-product"> {
  let summary = "Replace separate @compute and @constrain functions in a "
                "struct with a single @product function";
  let description = summary;
  let options = [Option<"rootStruct", "root-struct", "std::string",
                        /* default */ "\"Main\"",
                        "Root struct at which to start alignment "
                        "(default to `@Main`)">];
}

def FuseProductControlFlowPass : LLZKPass<"llzk-fuse-product-control-flow"> {
  let summary =
      "Fuse matching compute/constrain control flow in a @product function";
  let description = [{
    Fuse uniquely matched compute/constrain `scf.for` and `scf.if` siblings in `@product`
    functions. A pair must have the same parent region and opposite `product_source` roles.

    Loop pairs must identify the same lower bound, upper bound, and step and use the same comparison
    mode: an absent or false `unsignedCmp` attribute is signed, while true is unsigned. Intervening
    compute operations may move after the constrain loop only when they are direct member writes or
    MLIR proves them recursively pure; other intervening operations must be recursively pure. A
    moved member write cannot cross observable storage effects, calls, allocation, traps,
    unsupported structured control flow, or unknown effects in the constrain loop. Moving any
    result must also preserve dominance. Fusion interleaves the loop bodies, so every loop pair
    requires a recursively pure compute loop and a constrain loop with none of those unsafe
    operations.

    Conditional pairs must be in the same block with the compute op first, use the exact same SSA
    condition, have matching else regions and compatible attributes, and have a resultless
    constrain op. Each fused branch keeps compute operations before constrain operations and
    preserves the compute results. Between the pair, the pass permits only direct member writes of
    compute-if results and matching unoffset constrain-side signal reads. Each such read must have
    operands available before the compute op, be used only by the constrain op, and match exactly
    one intervening write; other intervening operations prevent fusion.

    Fused operations preserve compatible attributes and `scf.yield` attributes, loop comparison
    mode, and combined source locations, and are marked `product_source = "fused"`. Unsafe,
    ambiguous, and unmatched control flow is left unchanged.
  }];
}

def EnforceNoMemberOverwritePass : LLZKPass<"llzk-enforce-no-overwrite"> {
  let summary = "Checks that every struct member is written exactly once";
  let description = [{
      This pass currently reports an error if any struct member may not be written
      exactly once (i.e. overwritten or left uninitialized), and does not
      attempt to perform any repairs (e.g. SSA-ifying overwritten struct
      members, or default-initializing unwritten members). This pass
      overapproximates conditionals, and can result in false positives.
  }];
}

def InlineFreeFunctionsPass : LLZKPass<"llzk-inline-free-functions"> {
  let summary = "Inline calls to free functions";
  let description = [{
    Inlines `function.call` operations inside `function.def` bodies when the
    target is a free `function.def` directly inside the pass's root
    `builtin.module`. Calls to functions in nested or included modules, and
    calls outside `function.def` bodies, are left unchanged. After inlining,
    root free functions that have no remaining symbol uses anywhere in the
    module are erased. This cleanup applies to every non-external root free
    function, regardless of its symbol visibility or whether any call to it
    was inlined.

    Inlining is best effort: free functions that participate in a call
    cycle are skipped (inlining one would re-materialize its calls
    forever). If inlining a callee fails, its remaining calls are left in
    place with a warning while processing continues with other callees.
  }];
}

def WhileToForPass : LLZKPass<"llzk-while-to-for"> {
  let summary =
      "Converts scf.while loops to equivalent scf.for loops when possible";
  let description =
      "This pass identifies scf.while loops that have an "
      "induction variable and a uniform step, and converts them "
      "to equivalent scf.for loops, which are preferred by some "
      "analyses. This pass may introduce some spurious felt/index casts which "
      "should be cleaned up by --canonicalize.";
}

#endif // LLZK_TRANSFORMATION_PASSES_TD
