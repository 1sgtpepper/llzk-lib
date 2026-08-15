//===-- FuseProductControlFlowTests.cpp - Control-flow fusion tests -*- C++ -*-===//
//
// Part of the LLZK Project, under the Apache License v2.0.
// See LICENSE.txt for license information.
// Copyright 2026 Project LLZK
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//

#include "../LLZKTestBase.h"

#include "llzk/Dialect/Function/IR/Ops.h"
#include "llzk/Dialect/Struct/IR/Ops.h"
#include "llzk/Transforms/LLZKTransformationPasses.h"

#include <mlir/Dialect/SCF/IR/SCF.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/Parser/Parser.h>
#include <mlir/Pass/PassManager.h>

#include <llvm/ADT/SmallVector.h>

namespace {

class FuseProductControlFlowTests : public LLZKTest {};

TEST_F(FuseProductControlFlowTests, HoistedMemberReadsPreserveSourceOrder) {
  mlir::OwningOpRef<mlir::ModuleOp> module = mlir::parseSourceString<mlir::ModuleOp>(
      R"mlir(
    module attributes {llzk.lang = "llzk"} {
      struct.def @A {
        struct.member @left : !felt.type
        struct.member @right : !felt.type

        function.def @product(%condition: i1) -> !struct.type<@A> {
          %self = struct.new : <@A>

          %left, %right = scf.if %condition -> (!felt.type, !felt.type) {
            %zero = felt.const 0
            %one = felt.const 1
            scf.yield %zero, %one : !felt.type, !felt.type
          } else {
            %two = felt.const 2
            %three = felt.const 3
            scf.yield %two, %three : !felt.type, !felt.type
          } {product_source = "compute"}

          struct.writem %self[@left] = %left : <@A>, !felt.type
          struct.writem %self[@right] = %right : <@A>, !felt.type
          %left_read = struct.readm %self[@left] : <@A>, !felt.type {
            product_source = "constrain"
          }
          %right_read = struct.readm %self[@right] : <@A>, !felt.type {
            product_source = "constrain"
          }

          scf.if %condition {
            %expected_left = felt.const 0
            %expected_right = felt.const 1
            constrain.eq %left_read, %expected_left : !felt.type, !felt.type
            constrain.eq %right_read, %expected_right : !felt.type, !felt.type
          } else {
            %expected_left = felt.const 2
            %expected_right = felt.const 3
            constrain.eq %left_read, %expected_left : !felt.type, !felt.type
            constrain.eq %right_read, %expected_right : !felt.type, !felt.type
          } {product_source = "constrain"}

          function.return %self : !struct.type<@A>
        }
      }
    }
  )mlir",
      &ctx
  );
  ASSERT_TRUE(module);

  mlir::PassManager pm(&ctx);
  pm.addPass(llzk::createFuseProductControlFlowPass());
  ASSERT_TRUE(mlir::succeeded(pm.run(*module)));

  llzk::function::FuncDefOp product;
  module->walk([&](llzk::function::FuncDefOp func) {
    if (func.isStructProduct()) {
      product = func;
    }
  });
  ASSERT_TRUE(product);

  llvm::SmallVector<llzk::component::MemberReadOp> reads;
  mlir::scf::IfOp fusedIf;
  for (mlir::Operation &op : product.getBody().front()) {
    if (auto read = llvm::dyn_cast<llzk::component::MemberReadOp>(&op)) {
      reads.push_back(read);
    } else if (auto ifOp = llvm::dyn_cast<mlir::scf::IfOp>(&op)) {
      if (auto source = ifOp->getAttrOfType<mlir::StringAttr>("product_source");
          source && source.getValue() == "fused") {
        fusedIf = ifOp;
      }
    }
  }

  ASSERT_EQ(reads.size(), 2U);
  ASSERT_TRUE(fusedIf);
  EXPECT_EQ(reads[0].getMemberName(), "left");
  EXPECT_EQ(reads[1].getMemberName(), "right");
  EXPECT_TRUE(reads[0]->isBeforeInBlock(reads[1]));
  EXPECT_TRUE(reads[0]->isBeforeInBlock(fusedIf));
  EXPECT_TRUE(reads[1]->isBeforeInBlock(fusedIf));
}

} // namespace
