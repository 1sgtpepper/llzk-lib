//===-- SignalEquivalenceMemberIdentityVerifyTests.cpp ----------*- C++ -*-===//
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

TEST_F(LLZKTest, MemberReadReplacementRespectsMemberIdentity) {
  mlir::OwningOpRef<mlir::ModuleOp> module = mlir::parseSourceString<mlir::ModuleOp>(
      R"mlir(
    module attributes {llzk.lang = "llzk"} {
      struct.def @Pair {
        struct.member @left : !felt.type
        struct.member @right : !felt.type

        function.def @product(%left_value: !felt.type, %right_value: !felt.type)
            -> !struct.type<@Pair>
            attributes {function.allow_constraint, function.allow_witness} {
          %self = struct.new : !struct.type<@Pair>
          struct.writem %self[@left] = %left_value : !struct.type<@Pair>, !felt.type
          struct.writem %self[@right] = %right_value : !struct.type<@Pair>, !felt.type
          %left = struct.readm %self[@left] : !struct.type<@Pair>, !felt.type
          %right = struct.readm %self[@right] : !struct.type<@Pair>, !felt.type
          function.return %self : !struct.type<@Pair>
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

  // @left read must not resolve to the intervening @right write.
  llzk::LightweightSignalEquivalenceAnalysis nonMatch(module->getOperation());
  EXPECT_FALSE(nonMatch.areSignalsEquivalent(leftRead.getVal(), rightValue));

  // @left read must resolve to the earlier @left write.
  llzk::LightweightSignalEquivalenceAnalysis leftMatch(module->getOperation());
  EXPECT_TRUE(leftMatch.areSignalsEquivalent(leftRead.getVal(), leftValue));

  // @right read still resolves to the @right write.
  llzk::LightweightSignalEquivalenceAnalysis rightMatch(module->getOperation());
  EXPECT_TRUE(rightMatch.areSignalsEquivalent(rightRead.getVal(), rightValue));
}

} // namespace
