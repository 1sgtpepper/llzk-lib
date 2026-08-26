//===-- SignalEquivalenceMemberIdentityReproTests.cpp -----------*- C++ -*-===//
//
// Part of the LLZK Project, under the Apache License v2.0.
// See LICENSE.txt for license information.
// Copyright 2026 Project LLZK
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//

#include "../LLZKTestBase.h"

#include "llzk/Analysis/LightweightSignalEquivalenceAnalysis.h"
#include "llzk/Dialect/Struct/IR/Ops.h"

#include <mlir/IR/BuiltinOps.h>
#include <mlir/Parser/Parser.h>

namespace {

TEST_F(LLZKTest, MemberReadReplacementIgnoresMemberIdentity) {
  mlir::OwningOpRef<mlir::ModuleOp> module = mlir::parseSourceString<mlir::ModuleOp>(
      R"mlir(
    module attributes {llzk.lang = "llzk"} {
      struct.def @Pair {
        struct.member @left : !felt.type
        struct.member @right : !felt.type

        function.def @probe(%left_value: !felt.type, %right_value: !felt.type)
            attributes {function.allow_witness} {
          %self = struct.new : !struct.type<@Pair>
          struct.writem %self[@left] = %left_value : !struct.type<@Pair>, !felt.type
          struct.writem %self[@right] = %right_value : !struct.type<@Pair>, !felt.type
          %left = struct.readm %self[@left] : !struct.type<@Pair>, !felt.type
          %right = struct.readm %self[@right] : !struct.type<@Pair>, !felt.type
          function.return
        }
      }
    }
  )mlir",
      &ctx
  );
  ASSERT_TRUE(module);

  llzk::component::MemberReadOp leftRead;
  llzk::component::MemberReadOp rightRead;
  module->walk([&](llzk::component::MemberReadOp read) {
    if (read.getMemberName() == "left") {
      leftRead = read;
    } else if (read.getMemberName() == "right") {
      rightRead = read;
    }
  });
  ASSERT_TRUE(leftRead);
  ASSERT_TRUE(rightRead);

  mlir::Block *block = leftRead->getBlock();
  ASSERT_EQ(block->getNumArguments(), 2U);
  mlir::Value leftValue = block->getArgument(0);
  mlir::Value rightValue = block->getArgument(1);

  // The nearest preceding write targets @right, so the analysis incorrectly substitutes its value
  // for the read of @left.
  llzk::LightweightSignalEquivalenceAnalysis falsePositive(module->getOperation());
  EXPECT_TRUE(falsePositive.areSignalsEquivalent(leftRead.getVal(), rightValue));

  llzk::LightweightSignalEquivalenceAnalysis missedMatch(module->getOperation());
  EXPECT_FALSE(missedMatch.areSignalsEquivalent(leftRead.getVal(), leftValue));

  // A read of the member targeted by the nearest write remains a valid control.
  llzk::LightweightSignalEquivalenceAnalysis matchingControl(module->getOperation());
  EXPECT_TRUE(matchingControl.areSignalsEquivalent(rightRead.getVal(), rightValue));
}

} // namespace
