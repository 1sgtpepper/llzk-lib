//===-- SignalEquivalenceReproTests.cpp - Equivalence repro ----*- C++ -*-===//
//
// Part of the LLZK Project, under the Apache License v2.0.
// See LICENSE.txt for license information.
// Copyright 2026 Project LLZK
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//

#include "../LLZKTestBase.h"

#include "llzk/Analysis/LightweightSignalEquivalenceAnalysis.h"
#include "llzk/Dialect/Function/IR/Ops.h"

#include <mlir/Dialect/Arith/IR/Arith.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/Parser/Parser.h>

#include <llvm/ADT/SmallVector.h>

namespace {

TEST_F(LLZKTest, ComparisonPredicateIsMissingFromSignalIdentity) {
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
  EXPECT_TRUE(
      equivalence.areSignalsEquivalent(comparisons[0].getResult(), comparisons[1].getResult())
  );

  // This captures the unsafe current behavior: operation attributes are absent from the identity,
  // so `eq` and `ne` compare as equivalent when their operands match.
  EXPECT_TRUE(
      equivalence.areSignalsEquivalent(comparisons[0].getResult(), comparisons[2].getResult())
  );
  EXPECT_FALSE(
      equivalence.areSignalsEquivalent(comparisons[0].getResult(), comparisons[3].getResult())
  );
}

} // namespace
