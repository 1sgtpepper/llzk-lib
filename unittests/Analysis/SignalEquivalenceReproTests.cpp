//===-- SignalEquivalenceReproTests.cpp - Issue 700 probe -------*- C++ -*-===//
//
// Part of the LLZK Project, under the Apache License v2.0.
// See LICENSE.txt for license information.
// Copyright 2026 Project LLZK
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//

#include "../LLZKTestBase.h"

#include "llzk/Analysis/LightweightSignalEquivalenceAnalysis.h"

#include <mlir/Dialect/Arith/IR/Arith.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/Parser/Parser.h>

#include <llvm/ADT/SmallVector.h>

#include <cstdio>

namespace {

TEST_F(LLZKTest, Issue700ComparisonPredicateProbe) {
  mlir::OwningOpRef<mlir::ModuleOp> module = mlir::parseSourceString<mlir::ModuleOp>(
      R"mlir(
    module attributes {llzk.lang = "llzk"} {
      function.def @probe(%lhs: index, %rhs: index, %other: index) {
        %same_a = arith.cmpi eq, %lhs, %rhs : index
        %same_b = arith.cmpi eq, %lhs, %rhs : index
        %different_predicate = arith.cmpi ne, %lhs, %rhs : index
        %different_operand = arith.cmpi eq, %lhs, %other : index
        function.return
      }
    }
  )mlir",
      &ctx
  );
  ASSERT_TRUE(module);

  llvm::SmallVector<mlir::arith::CmpIOp> comparisons;
  module->walk([&](mlir::arith::CmpIOp comparison) { comparisons.push_back(comparison); });
  ASSERT_EQ(comparisons.size(), 4U);

  llzk::LightweightSignalEquivalenceAnalysis equivalence(module->getOperation());
  const bool sameEq = equivalence.areSignalsEquivalent(
      comparisons[0].getResult(), comparisons[1].getResult()
  );
  const bool eqVsNe = equivalence.areSignalsEquivalent(
      comparisons[0].getResult(), comparisons[2].getResult()
  );
  const bool diffOperand = equivalence.areSignalsEquivalent(
      comparisons[0].getResult(), comparisons[3].getResult()
  );

  std::printf(
      "ISSUE_700_PROBE sameEq=%s eqVsNe=%s diffOperand=%s\n",
      sameEq ? "true" : "false", eqVsNe ? "true" : "false", diffOperand ? "true" : "false"
  );

  EXPECT_TRUE(sameEq);
  EXPECT_FALSE(diffOperand);
}

} // namespace
