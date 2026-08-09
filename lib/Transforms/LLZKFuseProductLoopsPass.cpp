//===-- LLZKFuseProductLoopsPass.cpp ----------------------------*- C++ -*-===//
//
// Part of the LLZK Project, under the Apache License v2.0.
// See LICENSE.txt for license information.
// Copyright 2026 Project LLZK
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements the `-llzk-fuse-product-loops` pass.
///
//===----------------------------------------------------------------------===//

#include "llzk/Dialect/Constrain/IR/Ops.h"
#include "llzk/Dialect/Felt/IR/Ops.h"
#include "llzk/Dialect/Function/IR/Ops.h"
#include "llzk/Dialect/Global/IR/Ops.h"
#include "llzk/Dialect/LLZK/IR/Ops.h"
#include "llzk/Dialect/Polymorphic/IR/Ops.h"
#include "llzk/Dialect/RAM/IR/Ops.h"
#include "llzk/Dialect/Struct/IR/Ops.h"
#include "llzk/Transforms/LLZKTransformationPasses.h"
#include "llzk/Util/AlignmentHelper.h"
#include "llzk/Util/Constants.h"
#include "llzk/Util/ProductSourceHelper.h"

#include <mlir/Dialect/SCF/Utils/Utils.h>
#include <mlir/IR/BuiltinAttributes.h>
#include <mlir/IR/IRMapping.h>
#include <mlir/IR/PatternMatch.h>
#include <mlir/Interfaces/SideEffectInterfaces.h>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/SMTAPI.h>

#include <memory>
#include <optional>

// Include the generated base pass class definitions.
namespace llzk {
#define GEN_PASS_DEF_FUSEPRODUCTLOOPSPASS
#include "llzk/Transforms/LLZKTransformationPasses.h.inc"
} // namespace llzk

namespace {

using namespace mlir;
using namespace llzk;
using namespace llzk::component;

static LogicalResult fuseMatchingRegionControlFlow(Region &body, MLIRContext *context);
static inline bool areOppositeProductSources(Operation *a, Operation *b);

// Bitwidth of `index` for instantiating SMT variables
constexpr int INDEX_WIDTH = 64;

static inline bool isConstOrStructParam(Value val) {
  // TODO: doing arithmetic over constants should also be fine?
  return llvm::isa<arith::ConstantIndexOp, polymorphic::ConstReadOp, felt::FeltConstantOp>(
      val.getDefiningOp()
  );
}

static llvm::SMTExprRef mkExpr(Value value, llvm::SMTSolver *solver) {
  if (auto constOp = value.getDefiningOp<arith::ConstantIndexOp>()) {
    return solver->mkBitvector(llvm::APSInt::get(constOp.value()), INDEX_WIDTH);
  } else if (auto polyReadOp = value.getDefiningOp<polymorphic::ConstReadOp>()) {

    return solver->mkSymbol(
        std::string {polyReadOp.getConstName()}.c_str(), solver->getBitvectorSort(INDEX_WIDTH)
    );
  }
  assert(false && "unsupported: checking non-constant trip counts");
  return nullptr; // Unreachable
}

static llvm::SMTExprRef tripCount(scf::ForOp op, llvm::SMTSolver *solver) {
  const auto *one = solver->mkBitvector(llvm::APSInt::get(1), INDEX_WIDTH);
  return solver->mkBVSDiv(
      solver->mkBVAdd(
          one,
          solver->mkBVSub(mkExpr(op.getUpperBound(), solver), mkExpr(op.getLowerBound(), solver))
      ),
      mkExpr(op.getStep(), solver)
  );
}

static inline bool canLoopsBeFused(scf::ForOp a, scf::ForOp b) {
  // A priori, two loops can be fused if:
  // 1. They live in the same parent region,
  // 2. One is compute-sourced and the other is constrain-sourced, and
  // 3. They have the same trip count

  // Check 1.
  if (a->getParentRegion() != b->getParentRegion()) {
    return false;
  }

  // Check 2.
  if (!areOppositeProductSources(a, b)) {
    return false;
  }

  // Check 3.
  // Easy case: both have a constant trip-count. If the trip counts are not "constant up to a struct
  // param", we definitely can't tell if they're equal. If the trip counts are only "constant up to
  // a struct param" but not actually constant, we can ask a solver if the equations are guaranteed
  // to be the same
  auto tripCountA = constantTripCount(a.getLowerBound(), a.getUpperBound(), a.getStep());
  auto tripCountB = constantTripCount(b.getLowerBound(), b.getUpperBound(), b.getStep());
  if (tripCountA.has_value() && tripCountB.has_value() && *tripCountA == *tripCountB) {
    return true;
  }

  if (!isConstOrStructParam(a.getLowerBound()) || !isConstOrStructParam(a.getUpperBound()) ||
      !isConstOrStructParam(a.getStep()) || !isConstOrStructParam(b.getLowerBound()) ||
      !isConstOrStructParam(b.getUpperBound()) || !isConstOrStructParam(b.getStep())) {
    return false;
  }

  llvm::SMTSolverRef solver = llvm::CreateZ3Solver();
  solver->addConstraint(/* (actually ask if they "can't be different") */ solver->mkNot(
      solver->mkEqual(tripCount(a, solver.get()), tripCount(b, solver.get()))
  ));

  return !*solver->check();
}

/// Return whether one operation is compute-sourced and the other is constrain-sourced.
static inline bool areOppositeProductSources(Operation *a, Operation *b) {
  std::optional<llvm::StringRef> sourceA = getProductSource(a);
  std::optional<llvm::StringRef> sourceB = getProductSource(b);
  if (!sourceA || !sourceB) {
    return false;
  }
  return (*sourceA == FUNC_NAME_COMPUTE && *sourceB == FUNC_NAME_CONSTRAIN) ||
         (*sourceA == FUNC_NAME_CONSTRAIN && *sourceB == FUNC_NAME_COMPUTE);
}

/// Return whether `op` lies strictly between `before` and `after` in the same block.
static bool isBetweenInBlock(Operation *op, Operation *before, Operation *after) {
  return op->getBlock() == before->getBlock() && op->getBlock() == after->getBlock() &&
         before->isBeforeInBlock(op) && op->isBeforeInBlock(after);
}

/// Return the result number of `value` on `ifOp`, if `value` is one of its results.
static std::optional<unsigned> getIfResultIndex(scf::IfOp ifOp, Value value) {
  for (auto [idx, result] : llvm::enumerate(ifOp.getResults())) {
    if (result == value) {
      return idx;
    }
  }
  return std::nullopt;
}

/// Return whether `op` may move with the constrain branch across compute-side operations.
static bool isSafeToMoveConstrainOp(Operation *op) {
  // ConstraintOpInterface identifies constraint-producing operations but does not guarantee
  // movement safety. Keep this whitelist explicit until the interface carries that contract.
  if (isa<llzk::constrain::EmitEqualityOp, llzk::constrain::EmitContainmentOp, llzk::NonDetOp>(
          op
      )) {
    return true;
  }

  // Nested operations are checked by the walk separately. Admit only the structured control-flow
  // operations this pass recurses into; scf.for must also pass MLIR's speculatability check.
  if (isa<scf::IfOp>(op)) {
    return true;
  }
  if (isa<scf::WhileOp>(op)) {
    return false;
  }
  if (auto forOp = dyn_cast<scf::ForOp>(op)) {
    return forOp.getSpeculatability() != Speculation::NotSpeculatable;
  }

  return isPure(op);
}

/// Return attributes that can be preserved on an operation created by if fusion.
///
/// `product_source` identifies the source role of each input operation and cannot be copied to
/// the operation that combines both roles. All other attributes must agree exactly; otherwise the
/// transformation declines to guess which source attribute semantics apply to the fused operation.
static std::optional<DictionaryAttr> getCompatibleFusedAttrs(Operation *a, Operation *b) {
  NamedAttrList attrsA(a->getAttrs());
  attrsA.erase(PRODUCT_SOURCE);
  NamedAttrList attrsB(b->getAttrs());
  attrsB.erase(PRODUCT_SOURCE);
  if (attrsA != attrsB) {
    return std::nullopt;
  }
  return attrsA.getDictionary(a->getContext());
}

/// Return whether the constrain branch contains an operation unsafe to move across compute-side
/// operations.
///
/// The branch is cloned into the earlier compute branch. An operation is movable only when this
/// pass explicitly admits it or MLIR proves it pure; the walk rejects reads, writes, calls, traps,
/// and unknown effects.
static bool hasUnsafeMovedConstrainOp(scf::IfOp constrainIf) {
  auto result = constrainIf->walk([&](Operation *op) {
    if (op == constrainIf.getOperation() || isa<scf::YieldOp>(op)) {
      return WalkResult::advance();
    }

    if (!isSafeToMoveConstrainOp(op)) {
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  return result.wasInterrupted();
}

/// Collect the compute-if result mappings needed by `constrainIf` and reject unsafe crossings.
///
/// The mapping lets cloned constrain operands use branch-local compute values; return false when
/// intervening definitions or effects would make the move observable.
static bool collectConstrainValueMappings(
    scf::IfOp computeIf, scf::IfOp constrainIf, llvm::DenseMap<Value, unsigned> &valueToResult
) {
  // Fusion clones the constrain branch before direct compute-result member writes. Only member
  // writes whose value is a direct compute-if result are currently proven safe to cross; member
  // reads, other storage operations, and intervening operations are rejected.
  for (Operation *op = computeIf->getNextNode(); op != constrainIf; op = op->getNextNode()) {
    if (auto writeOp = dyn_cast<MemberWriteOp>(op)) {
      std::optional<unsigned> resultIndex = getIfResultIndex(computeIf, writeOp.getVal());
      if (!resultIndex) {
        return false;
      }
      continue;
    }

    if (isa<MemberReadOp, llzk::global::GlobalReadOp, llzk::global::GlobalWriteOp,
            llzk::ram::LoadOp, llzk::ram::StoreOp>(op)) {
      // Moving the constrain branch across storage access changes the state it observes. Replacing
      // a member read with a branch-local value would also remove that member signal from emitted
      // constraints.
      return false;
    }

    // No other operation between the sibling ifs is currently proven safe to cross.
    return false;
  }

  for (auto [idx, result] : llvm::enumerate(computeIf.getResults())) {
    valueToResult[result] = idx;
  }

  if (hasUnsafeMovedConstrainOp(constrainIf)) {
    return false;
  }

  auto result = constrainIf->walk([&](Operation *op) {
    for (Value operand : op->getOperands()) {
      Operation *def = operand.getDefiningOp();
      if (!def || constrainIf->isAncestor(def)) {
        continue;
      }
      if (valueToResult.contains(operand)) {
        continue;
      }
      if (!isBetweenInBlock(def, computeIf, constrainIf)) {
        continue;
      }
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  return !result.wasInterrupted();
}

/// Return whether two marked sibling `scf.if` ops satisfy the conservative fusion contract.
static bool canIfsBeFused(scf::IfOp a, scf::IfOp b) {
  if (a->getBlock() != b->getBlock()) {
    return false;
  }
  if (!areOppositeProductSources(a, b)) {
    return false;
  }

  scf::IfOp computeIf = hasProductSource(a, FUNC_NAME_COMPUTE) ? a : b;
  scf::IfOp constrainIf = computeIf == a ? b : a;
  if (!computeIf->isBeforeInBlock(constrainIf)) {
    return false;
  }
  if (!constrainIf->getResults().empty()) {
    return false;
  }
  if (computeIf.getElseRegion().empty() != constrainIf.getElseRegion().empty()) {
    return false;
  }
  if (computeIf.getCondition() != constrainIf.getCondition()) {
    return false;
  }
  if (!getCompatibleFusedAttrs(computeIf, constrainIf)) {
    return false;
  }
  if (!getCompatibleFusedAttrs(
          computeIf.thenBlock()->getTerminator(), constrainIf.thenBlock()->getTerminator()
      )) {
    return false;
  }
  if (!computeIf.getElseRegion().empty() &&
      !getCompatibleFusedAttrs(
          computeIf.elseBlock()->getTerminator(), constrainIf.elseBlock()->getTerminator()
      )) {
    return false;
  }

  llvm::DenseMap<Value, unsigned> valueToResult;
  return collectConstrainValueMappings(computeIf, constrainIf, valueToResult);
}

/// Remove the destination block's existing `scf.yield` before appending a cloned branch.
static void eraseDefaultTerminator(Block *block) {
  if (!block->empty()) {
    if (auto yieldOp = dyn_cast<scf::YieldOp>(block->back())) {
      yieldOp.erase();
    }
  }
}

/// Clone compute and constrain branch bodies into `destBlock` and rebuild a compatible yield.
/// Compute results are remapped to the branch-local values used by constrain operations.
static void cloneIfBranch(
    Block *computeBlock, Block *constrainBlock, Block *destBlock,
    const llvm::DenseMap<Value, unsigned> &valueToResult, OpBuilder &builder
) {
  eraseDefaultTerminator(destBlock);
  IRMapping mapper;
  builder.setInsertionPointToEnd(destBlock);

  scf::YieldOp computeYield = cast<scf::YieldOp>(computeBlock->getTerminator());
  scf::YieldOp constrainYield = cast<scf::YieldOp>(constrainBlock->getTerminator());
  for (Operation &op : computeBlock->without_terminator()) {
    builder.clone(op, mapper);
  }
  for (auto [value, resultIndex] : valueToResult) {
    Value branchValue = computeYield.getResults()[resultIndex];
    mapper.map(value, mapper.lookupOrDefault(branchValue));
  }
  for (Operation &op : constrainBlock->without_terminator()) {
    builder.clone(op, mapper);
  }

  llvm::SmallVector<Value> yieldOperands;
  yieldOperands.reserve(computeYield.getResults().size());
  for (Value operand : computeYield.getResults()) {
    yieldOperands.push_back(mapper.lookupOrDefault(operand));
  }
  auto fusedYield = builder.create<scf::YieldOp>(
      builder.getFusedLoc({computeYield.getLoc(), constrainYield.getLoc()}), yieldOperands
  );
  std::optional<DictionaryAttr> yieldAttrs = getCompatibleFusedAttrs(computeYield, constrainYield);
  assert(yieldAttrs && "fusion candidates must have compatible yield attributes");
  fusedYield->setAttrs(*yieldAttrs);
}

/// Replace a checked compute/constrain `scf.if` pair with one fused `scf.if`.
static LogicalResult
fuseIfPair(scf::IfOp a, scf::IfOp b, MLIRContext *context, IRRewriter &rewriter) {
  scf::IfOp computeIf = hasProductSource(a, FUNC_NAME_COMPUTE) ? a : b;
  scf::IfOp constrainIf = computeIf == a ? b : a;

  llvm::DenseMap<Value, unsigned> valueToResult;
  [[maybe_unused]] bool canMap =
      collectConstrainValueMappings(computeIf, constrainIf, valueToResult);
  assert(canMap && "fusion candidates must have already been checked");

  rewriter.setInsertionPoint(computeIf);
  std::optional<DictionaryAttr> fusedAttrs = getCompatibleFusedAttrs(computeIf, constrainIf);
  assert(fusedAttrs && "fusion candidates must have compatible if attributes");
  scf::IfOp fusedIf = rewriter.create<scf::IfOp>(
      rewriter.getFusedLoc({computeIf.getLoc(), constrainIf.getLoc()}), computeIf.getResultTypes(),
      computeIf.getCondition(), !computeIf.getElseRegion().empty()
  );
  fusedIf->setAttrs(*fusedAttrs);
  setProductSource(fusedIf, "fused");

  cloneIfBranch(
      computeIf.thenBlock(), constrainIf.thenBlock(), fusedIf.thenBlock(), valueToResult, rewriter
  );
  if (!computeIf.getElseRegion().empty()) {
    cloneIfBranch(
        computeIf.elseBlock(), constrainIf.elseBlock(), fusedIf.elseBlock(), valueToResult, rewriter
    );
  }

  // Recursive fusion runs after cloning; roll back the provisional op before reporting failure.
  if (failed(fuseMatchingRegionControlFlow(fusedIf.getThenRegion(), context))) {
    rewriter.eraseOp(fusedIf);
    return failure();
  }
  if (!fusedIf.getElseRegion().empty() &&
      failed(fuseMatchingRegionControlFlow(fusedIf.getElseRegion(), context))) {
    rewriter.eraseOp(fusedIf);
    return failure();
  }

  computeIf->replaceAllUsesWith(fusedIf->getResults());
  rewriter.eraseOp(constrainIf);
  rewriter.eraseOp(computeIf);
  return success();
}

/// Fuse uniquely matchable marked compute/constrain `scf.if` pairs in `body`.
static LogicalResult fuseMatchingIfPairs(Region &body, MLIRContext *context) {
  llvm::SmallVector<scf::IfOp> witnessIfs, constraintIfs;
  body.walk<WalkOrder::PreOrder>([&](scf::IfOp ifOp) {
    std::optional<llvm::StringRef> productSource = getProductSource(ifOp);
    if (!productSource) {
      return WalkResult::advance();
    }
    if (*productSource == FUNC_NAME_COMPUTE) {
      witnessIfs.push_back(ifOp);
    } else if (*productSource == FUNC_NAME_CONSTRAIN) {
      constraintIfs.push_back(ifOp);
    }
    return WalkResult::skip();
  });

  auto fusionCandidates =
      alignmentHelpers::getMatchingPairs<scf::IfOp>(witnessIfs, constraintIfs, canIfsBeFused);
  if (failed(fusionCandidates)) {
    return failure();
  }

  IRRewriter rewriter {context};
  for (auto [w, c] : *fusionCandidates) {
    if (failed(fuseIfPair(w, c, context, rewriter))) {
      return failure();
    }
  }

  return success();
}

/// Collect compute-sourced operations between sibling loops that must move past `constraintLoop`.
/// Reject the region if a previously fused operation would cross the boundary.
static FailureOr<SmallVector<Operation *>>
canPrepareForFusion(scf::ForOp witnessLoop, scf::ForOp constraintLoop) {
  if (witnessLoop->getBlock() != constraintLoop->getBlock()) {
    return failure();
  }

  SmallVector<Operation *> opsToSink;
  for (auto *op = witnessLoop->getNextNode(); op != constraintLoop; op = op->getNextNode()) {
    if (hasProductSource(op, "fused")) {
      // "fused" means "compute" + "constrain". Conservatively, a "compute" op we want to sink can't
      // be sunk if it also has "constrain" since we need to preserve the relative orders within
      // compute/constrain
      return failure();
    }
    if (hasProductSource(op, FUNC_NAME_COMPUTE)) {
      opsToSink.push_back(op);
    }
  }
  return opsToSink;
}

static LogicalResult
prepareForFusion(scf::ForOp witnessLoop, scf::ForOp constraintLoop, IRRewriter &rewriter) {
  auto computeOpsToSink = canPrepareForFusion(witnessLoop, constraintLoop);
  if (failed(computeOpsToSink)) {
    return failure();
  }

  Operation *insertionPoint = constraintLoop.getOperation();
  for (Operation *op : *computeOpsToSink) {
    rewriter.moveOpAfter(op, insertionPoint);
    insertionPoint = op;
  }

  return success();
}

/// Fuse uniquely matchable marked compute/constrain `scf.for` pairs in `body`.
static LogicalResult fuseMatchingLoopPairs(Region &body, MLIRContext *context) {
  // Collect marked loops before matching unique compute/constrain pairs.
  llvm::SmallVector<scf::ForOp> witnessLoops, constraintLoops;
  body.walk<WalkOrder::PreOrder>([&witnessLoops, &constraintLoops](scf::ForOp forOp) {
    std::optional<llvm::StringRef> productSource = getProductSource(forOp);
    if (!productSource) {
      return WalkResult::skip();
    }
    if (*productSource == FUNC_NAME_COMPUTE) {
      witnessLoops.push_back(forOp);
    } else if (*productSource == FUNC_NAME_CONSTRAIN) {
      constraintLoops.push_back(forOp);
    }
    // Defer nested loops until their enclosing pair has been fused.
    return WalkResult::skip();
  });

  // A pair of loops will be fused iff (1) they can be fused according to the rules above, and (2)
  // neither can be fused with anything else (so there's no ambiguity)
  auto fusionCandidates = alignmentHelpers::getMatchingPairs<scf::ForOp>(
      witnessLoops, constraintLoops, canLoopsBeFused
  );

  // Preserve the failure path even though the matcher normally permits partial matches.
  if (failed(fusionCandidates)) {
    return failure();
  }

  // Fuse each unambiguous pair; leave preparation failures unchanged.
  IRRewriter rewriter {context};
  for (auto [w, c] : *fusionCandidates) {
    if (failed(prepareForFusion(w, c, rewriter))) {
      continue;
    }
    auto fusedLoop = fuseIndependentSiblingForLoops(w, c, rewriter);
    setProductSource(fusedLoop, "fused");
    // Recurse so nested if/loop pairs become eligible after loop fusion.
    if (failed(fuseMatchingRegionControlFlow(fusedLoop.getBodyRegion(), context))) {
      return failure();
    }
  }
  return success();
}

/// Fuse marked `scf.if` pairs and then `scf.for` pairs in `body`.
static LogicalResult fuseMatchingRegionControlFlow(Region &body, MLIRContext *context) {
  if (failed(fuseMatchingIfPairs(body, context))) {
    return failure();
  }
  return fuseMatchingLoopPairs(body, context);
}

class PassImpl : public llzk::impl::FuseProductLoopsPassBase<PassImpl> {
  using Base = FuseProductLoopsPassBase<PassImpl>;
  using Base::Base;

  void runOnOperation() override {
    ModuleOp mod = getOperation();
    mod.walk([this](function::FuncDefOp funcDef) {
      if (funcDef.isStructProduct()) {
        if (failed(fuseMatchingRegionControlFlow(funcDef.getFunctionBody(), &getContext()))) {
          signalPassFailure();
        }
      }
    });
  }
};

} // namespace
