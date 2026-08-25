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

TEST_F(FuseProductControlFlowTests, HoistedSignalMemberReadsPreserveSourceOrder) {
  // Distinct signal members make both source order and placement before the fused if observable.
  mlir::OwningOpRef<mlir::ModuleOp> module = mlir::parseSourceString<mlir::ModuleOp>(
      R"mlir(
    module attributes {llzk.lang = "llzk"} {
      struct.def @A {
        struct.member @left : !felt.type {signal}
        struct.member @right : !felt.type {signal}

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

TEST_F(FuseProductControlFlowTests, NonSignalMemberReadPreventsFusion) {
  // A non-signal member is an intermediate expression, so its read must stay after the write.
  mlir::OwningOpRef<mlir::ModuleOp> module = mlir::parseSourceString<mlir::ModuleOp>(
      R"mlir(
    module attributes {llzk.lang = "llzk"} {
      struct.def @A {
        struct.member @value : !felt.type

        function.def @product(%condition: i1) -> !struct.type<@A> {
          %self = struct.new : <@A>

          %value = scf.if %condition -> !felt.type {
            %zero = felt.const 0
            scf.yield %zero : !felt.type
          } else {
            %one = felt.const 1
            scf.yield %one : !felt.type
          } {product_source = "compute"}

          struct.writem %self[@value] = %value : <@A>, !felt.type
          %value_read = struct.readm %self[@value] : <@A>, !felt.type {
            product_source = "constrain"
          }

          scf.if %condition {
            %expected = felt.const 0
            constrain.eq %value_read, %expected : !felt.type, !felt.type
          } else {
            %expected = felt.const 1
            constrain.eq %value_read, %expected : !felt.type, !felt.type
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

  llzk::component::MemberWriteOp write;
  llzk::component::MemberReadOp read;
  mlir::scf::IfOp fusedIf;
  for (mlir::Operation &op : product.getBody().front()) {
    if (auto writeOp = llvm::dyn_cast<llzk::component::MemberWriteOp>(&op)) {
      write = writeOp;
    } else if (auto readOp = llvm::dyn_cast<llzk::component::MemberReadOp>(&op)) {
      read = readOp;
    } else if (auto ifOp = llvm::dyn_cast<mlir::scf::IfOp>(&op)) {
      if (auto source = ifOp->getAttrOfType<mlir::StringAttr>("product_source");
          source && source.getValue() == "fused") {
        fusedIf = ifOp;
      }
    }
  }

  ASSERT_TRUE(write);
  ASSERT_TRUE(read);
  EXPECT_FALSE(fusedIf);
  EXPECT_TRUE(write->isBeforeInBlock(read));
}

TEST_F(FuseProductControlFlowTests, UnmarkedSignalMemberReadPreventsFusion) {
  // A signal read without a constrain source marker is not eligible for hoisting across its write.
  mlir::OwningOpRef<mlir::ModuleOp> module = mlir::parseSourceString<mlir::ModuleOp>(
      R"mlir(
    module attributes {llzk.lang = "llzk"} {
      struct.def @A {
        struct.member @value : !felt.type {signal}

        function.def @product(%condition: i1) -> !struct.type<@A> {
          %self = struct.new : <@A>

          %value = scf.if %condition -> !felt.type {
            %zero = felt.const 0
            scf.yield %zero : !felt.type
          } else {
            %one = felt.const 1
            scf.yield %one : !felt.type
          } {product_source = "compute"}

          struct.writem %self[@value] = %value : <@A>, !felt.type
          %value_read = struct.readm %self[@value] : <@A>, !felt.type

          scf.if %condition {
            %expected = felt.const 0
            constrain.eq %value_read, %expected : !felt.type, !felt.type
          } else {
            %expected = felt.const 1
            constrain.eq %value_read, %expected : !felt.type, !felt.type
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

  llzk::component::MemberWriteOp write;
  llzk::component::MemberReadOp read;
  mlir::scf::IfOp fusedIf;
  for (mlir::Operation &op : product.getBody().front()) {
    if (auto writeOp = llvm::dyn_cast<llzk::component::MemberWriteOp>(&op)) {
      write = writeOp;
    } else if (auto readOp = llvm::dyn_cast<llzk::component::MemberReadOp>(&op)) {
      read = readOp;
    } else if (auto ifOp = llvm::dyn_cast<mlir::scf::IfOp>(&op)) {
      if (auto source = ifOp->getAttrOfType<mlir::StringAttr>("product_source");
          source && source.getValue() == "fused") {
        fusedIf = ifOp;
      }
    }
  }

  ASSERT_TRUE(write);
  ASSERT_TRUE(read);
  EXPECT_FALSE(fusedIf);
  EXPECT_TRUE(write->isBeforeInBlock(read));
}

TEST_F(FuseProductControlFlowTests, NestedLoopControlMismatchPreventsFusion) {
  // The outer conditionals may fuse, but equal-count loops with different bounds or steps must
  // retain each source induction sequence because their bodies observe the induction variable.
  mlir::OwningOpRef<mlir::ModuleOp> module = mlir::parseSourceString<mlir::ModuleOp>(
      R"mlir(
    module attributes {llzk.lang = "llzk"} {
      struct.def @A {
        struct.member @value : !felt.type

        function.def @product(%condition: i1) -> !struct.type<@A> {
          %self = struct.new : <@A>
          %c0 = arith.constant 0 : index
          %c1 = arith.constant 1 : index
          %c2 = arith.constant 2 : index
          %c3 = arith.constant 3 : index
          %zero = felt.const 0

          scf.if %condition {
            scf.for %i = %c0 to %c2 step %c1 {
              %lower_value = arith.addi %i, %c0 : index
              scf.yield
            } {product_source = "compute"}
            scf.for %i = %c0 to %c2 step %c2 {
              %step_value = arith.addi %i, %c0 : index
              scf.yield
            } {product_source = "compute"}
            scf.yield
          } {product_source = "compute"}

          scf.if %condition {
            scf.for %i = %c1 to %c3 step %c1 {
              %lower_value = arith.addi %i, %c0 : index
              scf.yield
            } {product_source = "constrain"}
            scf.for %i = %c0 to %c1 step %c1 {
              %step_value = arith.addi %i, %c0 : index
              scf.yield
            } {product_source = "constrain"}
            scf.yield
          } {product_source = "constrain"}

          struct.writem %self[@value] = %zero : <@A>, !felt.type
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

  unsigned fusedIfs = 0;
  unsigned computeLoops = 0;
  unsigned constrainLoops = 0;
  unsigned fusedLoops = 0;
  product.walk([&](mlir::scf::IfOp ifOp) {
    if (auto source = ifOp->getAttrOfType<mlir::StringAttr>("product_source");
        source && source.getValue() == "fused") {
      ++fusedIfs;
    }
  });
  product.walk([&](mlir::scf::ForOp loop) {
    mlir::StringAttr source = loop->getAttrOfType<mlir::StringAttr>("product_source");
    if (!source) {
      return;
    }
    if (source.getValue() == "compute") {
      ++computeLoops;
    } else if (source.getValue() == "constrain") {
      ++constrainLoops;
    } else if (source.getValue() == "fused") {
      ++fusedLoops;
    }
  });

  EXPECT_EQ(fusedIfs, 1U);
  EXPECT_EQ(computeLoops, 2U);
  EXPECT_EQ(constrainLoops, 2U);
  EXPECT_EQ(fusedLoops, 0U);
}

TEST_F(FuseProductControlFlowTests, ReversedLoopPairPreventsFusion) {
  // A constrain loop that precedes its compute partner is not a forward-sink candidate and must
  // remain unchanged instead of reaching the end of the block during preparation.
  mlir::OwningOpRef<mlir::ModuleOp> module = mlir::parseSourceString<mlir::ModuleOp>(
      R"mlir(
    module attributes {llzk.lang = "llzk"} {
      struct.def @A {
        struct.member @value : !felt.type

        function.def @product() -> !struct.type<@A> {
          %self = struct.new : <@A>
          %c0 = arith.constant 0 : index
          %c1 = arith.constant 1 : index
          %c2 = arith.constant 2 : index
          %zero = felt.const 0

          scf.for %i = %c0 to %c2 step %c1 {
            %observed = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "constrain"}

          scf.for %i = %c0 to %c2 step %c1 {
            %computed = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "compute"}

          struct.writem %self[@value] = %zero : <@A>, !felt.type
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

  mlir::scf::ForOp constrainLoop;
  mlir::scf::ForOp computeLoop;
  unsigned fusedLoops = 0;
  product.walk([&](mlir::scf::ForOp loop) {
    mlir::StringAttr source = loop->getAttrOfType<mlir::StringAttr>("product_source");
    if (!source) {
      return;
    }
    if (source.getValue() == "constrain") {
      constrainLoop = loop;
    } else if (source.getValue() == "compute") {
      computeLoop = loop;
    } else if (source.getValue() == "fused") {
      ++fusedLoops;
    }
  });

  ASSERT_TRUE(constrainLoop);
  ASSERT_TRUE(computeLoop);
  EXPECT_EQ(fusedLoops, 0U);
  EXPECT_TRUE(constrainLoop->isBeforeInBlock(computeLoop));
}

TEST_F(FuseProductControlFlowTests, CrossedLoopPairsUseLexicalApplicationOrder) {
  // The first pair moves the second compute loop past its constrain partner. The stale second
  // candidate becomes reversed; live-order revalidation blocks it, and lexical order chooses A.
  mlir::OwningOpRef<mlir::ModuleOp> module = mlir::parseSourceString<mlir::ModuleOp>(
      R"mlir(
    module attributes {llzk.lang = "llzk"} {
      struct.def @A {
        struct.member @value : !felt.type

        function.def @product() -> !struct.type<@A> {
          %self = struct.new : <@A>
          %c0 = arith.constant 0 : index
          %c1 = arith.constant 1 : index
          %c2 = arith.constant 2 : index
          %c3 = arith.constant 3 : index
          %zero = felt.const 0

          scf.for %i = %c0 to %c2 step %c1 {
            %compute_a = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "compute"}
          scf.for %i = %c0 to %c3 step %c1 {
            %compute_b = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "compute"}
          scf.for %i = %c0 to %c3 step %c1 {
            %constrain_b = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "constrain"}
          scf.for %i = %c0 to %c2 step %c1 {
            %constrain_a = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "constrain"}

          struct.writem %self[@value] = %zero : <@A>, !felt.type
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

  mlir::scf::ForOp fusedLoop;
  mlir::scf::ForOp remainingCompute;
  mlir::scf::ForOp remainingConstrain;
  product.walk([&](mlir::scf::ForOp loop) {
    mlir::StringAttr source = loop->getAttrOfType<mlir::StringAttr>("product_source");
    if (!source) {
      return;
    }
    if (source.getValue() == "fused") {
      fusedLoop = loop;
    } else if (source.getValue() == "compute") {
      remainingCompute = loop;
    } else if (source.getValue() == "constrain") {
      remainingConstrain = loop;
    }
  });

  ASSERT_TRUE(fusedLoop);
  ASSERT_TRUE(remainingCompute);
  ASSERT_TRUE(remainingConstrain);
  EXPECT_TRUE(remainingConstrain->isBeforeInBlock(fusedLoop));
  EXPECT_TRUE(fusedLoop->isBeforeInBlock(remainingCompute));
}

TEST_F(FuseProductControlFlowTests, LoopComparisonModeIsPartOfFusionIdentity) {
  // LLZK's witness interpreter treats unsignedCmp as loop semantics. Mixed modes stay separate,
  // while each equivalent representation (Unit, true, false, or absent) retains its effective
  // mode after the generic MLIR helper rebuilds the fused loop.
  mlir::OwningOpRef<mlir::ModuleOp> module = mlir::parseSourceString<mlir::ModuleOp>(
      R"mlir(
    module attributes {llzk.lang = "llzk"} {
      struct.def @A {
        struct.member @value : !felt.type

        function.def @product() -> !struct.type<@A> {
          %self = struct.new : <@A>
          %c0 = arith.constant 0 : index
          %c1 = arith.constant 1 : index
          %c2 = arith.constant 2 : index
          %c3 = arith.constant 3 : index
          %c4 = arith.constant 4 : index
          %c5 = arith.constant 5 : index
          %zero = felt.const 0

          scf.for %i = %c0 to %c2 step %c1 {
            %mixed_compute = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "compute", unsignedCmp}
          scf.for %i = %c0 to %c2 step %c1 {
            %mixed_constrain = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "constrain"}

          scf.for %i = %c0 to %c3 step %c1 {
            %equal_compute = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "compute", unsignedCmp}
          scf.for %i = %c0 to %c3 step %c1 {
            %equal_constrain = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "constrain", unsignedCmp}

          scf.for %i = %c0 to %c4 step %c1 {
            %false_compute = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "compute"}
          scf.for %i = %c0 to %c4 step %c1 {
            %false_constrain = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "constrain", unsignedCmp = false}

          scf.for %i = %c0 to %c5 step %c1 {
            %true_compute = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "compute", unsignedCmp = true}
          scf.for %i = %c0 to %c5 step %c1 {
            %true_constrain = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "constrain", unsignedCmp}

          struct.writem %self[@value] = %zero : <@A>, !felt.type
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

  unsigned computeLoops = 0;
  unsigned constrainLoops = 0;
  unsigned fusedLoops = 0;
  unsigned fusedUnsignedLoops = 0;
  unsigned fusedUnitLoops = 0;
  unsigned fusedTrueLoops = 0;
  unsigned fusedFalseLoops = 0;
  product.walk([&](mlir::scf::ForOp loop) {
    mlir::StringAttr source = loop->getAttrOfType<mlir::StringAttr>("product_source");
    if (!source) {
      return;
    }
    if (source.getValue() == "compute") {
      ++computeLoops;
    } else if (source.getValue() == "constrain") {
      ++constrainLoops;
    } else if (source.getValue() == "fused") {
      ++fusedLoops;
      if (auto boolAttr = loop->getAttrOfType<mlir::BoolAttr>("unsignedCmp")) {
        if (boolAttr.getValue()) {
          ++fusedTrueLoops;
          ++fusedUnsignedLoops;
        } else {
          ++fusedFalseLoops;
        }
      } else if (loop->hasAttr("unsignedCmp")) {
        ++fusedUnitLoops;
        ++fusedUnsignedLoops;
      }
    }
  });

  EXPECT_EQ(computeLoops, 1U);
  EXPECT_EQ(constrainLoops, 1U);
  EXPECT_EQ(fusedLoops, 3U);
  EXPECT_EQ(fusedUnsignedLoops, 2U);
  EXPECT_EQ(fusedUnitLoops, 1U);
  EXPECT_EQ(fusedTrueLoops, 1U);
  EXPECT_EQ(fusedFalseLoops, 1U);
}

TEST_F(FuseProductControlFlowTests, ComputeLoopResultDependenciesPreventFusion) {
  // A compute-loop result cannot move across a surviving constrain-loop user. The final pair is
  // the opposite branch: its compute-sourced pure user moves with the result and remains fusible.
  mlir::OwningOpRef<mlir::ModuleOp> module = mlir::parseSourceString<mlir::ModuleOp>(
      R"mlir(
    module attributes {llzk.lang = "llzk"} {
      struct.def @A {
        struct.member @value : !felt.type

        function.def @product() -> !struct.type<@A> {
          %self = struct.new : <@A>
          %c0 = arith.constant 0 : index
          %c1 = arith.constant 1 : index
          %c2 = arith.constant 2 : index
          %c3 = arith.constant 3 : index
          %c4 = arith.constant 4 : index
          %c5 = arith.constant 5 : index
          %c6 = arith.constant 6 : index
          %c7 = arith.constant 7 : index
          %zero = felt.const 0

          // The constrain iter_arg is initialized from the compute-loop result.
          %compute_iter = scf.for %i = %c0 to %c2 step %c1 iter_args(%acc = %c0) -> (index) {
            %next = arith.addi %acc, %c1 : index
            scf.yield %next : index
          } {product_source = "compute"}
          scf.for %i = %c0 to %c2 step %c1 iter_args(%acc = %compute_iter) -> (index) {
            %next = arith.addi %acc, %c1 : index
            scf.yield %next : index
          } {product_source = "constrain"}

          // A nested constrain bound captures the compute-loop result.
          %compute_bound = scf.for %i = %c0 to %c3 step %c1 iter_args(%acc = %c0) -> (index) {
            %next = arith.addi %acc, %c1 : index
            scf.yield %next : index
          } {product_source = "compute"}
          scf.for %i = %c0 to %c3 step %c1 {
            scf.for %j = %c0 to %compute_bound step %c1 {
              %nested = arith.addi %j, %c1 : index
              scf.yield
            }
            scf.yield
          } {product_source = "constrain"}

          // A pure operation in the constrain body captures the compute-loop result.
          %compute_body = scf.for %i = %c0 to %c4 step %c1 iter_args(%acc = %c0) -> (index) {
            %next = arith.addi %acc, %c1 : index
            scf.yield %next : index
          } {product_source = "compute"}
          scf.for %i = %c0 to %c4 step %c1 {
            %captured = arith.addi %compute_body, %c1 : index
            scf.yield
          } {product_source = "constrain"}

          // A constrain-sourced pure interstitial operation uses the result before the loop.
          %compute_tagged = scf.for %i = %c0 to %c5 step %c1 iter_args(%acc = %c0) -> (index) {
            %next = arith.addi %acc, %c1 : index
            scf.yield %next : index
          } {product_source = "compute"}
          %constrain_use = arith.addi %compute_tagged, %c1 {product_source = "constrain"} : index
          scf.for %i = %c0 to %c5 step %c1 {
            %unused_tagged = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "constrain"}

          // An unmarked pure interstitial operation has the same surviving-use restriction.
          %compute_unmarked = scf.for %i = %c0 to %c6 step %c1 iter_args(%acc = %c0) -> (index) {
            %next = arith.addi %acc, %c1 : index
            scf.yield %next : index
          } {product_source = "compute"}
          %unmarked_use = arith.addi %compute_unmarked, %c1 : index
          scf.for %i = %c0 to %c6 step %c1 {
            %unused_unmarked = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "constrain"}

          // The compute-sourced pure use moves after the constrain loop with its definition.
          %compute_sink = scf.for %i = %c0 to %c7 step %c1 iter_args(%acc = %c0) -> (index) {
            %next = arith.addi %acc, %c1 : index
            scf.yield %next : index
          } {product_source = "compute"}
          %sink_use = arith.addi %compute_sink, %c1 {product_source = "compute"} : index
          scf.for %i = %c0 to %c7 step %c1 {
            %unused_sink = arith.addi %i, %c0 : index
            scf.yield
          } {product_source = "constrain"}

          struct.writem %self[@value] = %zero : <@A>, !felt.type
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

  unsigned computeLoops = 0;
  unsigned constrainLoops = 0;
  unsigned fusedLoops = 0;
  product.walk([&](mlir::scf::ForOp loop) {
    mlir::StringAttr source = loop->getAttrOfType<mlir::StringAttr>("product_source");
    if (!source) {
      return;
    }
    if (source.getValue() == "compute") {
      ++computeLoops;
    } else if (source.getValue() == "constrain") {
      ++constrainLoops;
    } else if (source.getValue() == "fused") {
      ++fusedLoops;
    }
  });

  EXPECT_EQ(computeLoops, 5U);
  EXPECT_EQ(constrainLoops, 5U);
  EXPECT_EQ(fusedLoops, 1U);
}

TEST_F(FuseProductControlFlowTests, EffectfulOperationsBetweenLoopsPreventFusion) {
  // Global and RAM operations between the loops must keep their source-local order, whether the
  // interstitial write is constrain-sourced or unmarked.
  mlir::OwningOpRef<mlir::ModuleOp> module = mlir::parseSourceString<mlir::ModuleOp>(
      R"mlir(
    module attributes {llzk.lang = "llzk"} {
      global.def @g : !felt.type = 0

      struct.def @GlobalCase {
        struct.member @out : !felt.type

        function.def @product(%value: !felt.type) -> !struct.type<@GlobalCase> {
          %self = struct.new : <@GlobalCase>
          %c0 = arith.constant 0 : index
          %c1 = arith.constant 1 : index
          %c2 = arith.constant 2 : index
          %one = felt.const 1

          scf.for %i = %c0 to %c2 step %c1 {
            %computed = felt.add %value, %one
            scf.yield
          } {product_source = "compute"}

          %stored = felt.const 2 {product_source = "compute"}
          global.write @g = %stored : !felt.type {product_source = "compute"}

          scf.for %i = %c0 to %c2 step %c1 {
            %actual = global.read @g : !felt.type {product_source = "constrain"}
            constrain.eq %actual, %value : !felt.type
            scf.yield
          } {product_source = "constrain"}

          struct.writem %self[@out] = %value : <@GlobalCase>, !felt.type
          function.return %self : !struct.type<@GlobalCase>
        }
      }

      struct.def @RamCase {
        struct.member @out : !felt.type

        function.def @product(%value: !felt.type) -> !struct.type<@RamCase> {
          %self = struct.new : <@RamCase>
          %addr = arith.constant 0 : index
          %c0 = arith.constant 0 : index
          %c1 = arith.constant 1 : index
          %c2 = arith.constant 2 : index
          %one = felt.const 1

          scf.for %i = %c0 to %c2 step %c1 {
            %computed = felt.add %value, %one
            scf.yield
          } {product_source = "compute"}

          %stored = felt.const 2 {product_source = "compute"}
          ram.store %addr, %stored : !felt.type {product_source = "compute"}

          scf.for %i = %c0 to %c2 step %c1 {
            %actual = ram.load %addr : !felt.type {product_source = "constrain"}
            constrain.eq %actual, %value : !felt.type
            scf.yield
          } {product_source = "constrain"}

          struct.writem %self[@out] = %value : <@RamCase>, !felt.type
          function.return %self : !struct.type<@RamCase>
        }
      }

      struct.def @ConstrainEffectCase {
        struct.member @out : !felt.type

        function.def @product(%value: !felt.type) -> !struct.type<@ConstrainEffectCase> {
          %self = struct.new : <@ConstrainEffectCase>
          %c0 = arith.constant 0 : index
          %c1 = arith.constant 1 : index
          %c2 = arith.constant 2 : index
          %one = felt.const 1

          scf.for %i = %c0 to %c2 step %c1 {
            %computed = felt.add %value, %one
            scf.yield
          } {product_source = "compute"}

          %stored = felt.const 3
          global.write @g = %stored : !felt.type {product_source = "constrain"}

          scf.for %i = %c0 to %c2 step %c1 {
            %actual = global.read @g : !felt.type {product_source = "constrain"}
            constrain.eq %actual, %value : !felt.type
            scf.yield
          } {product_source = "constrain"}

          struct.writem %self[@out] = %value : <@ConstrainEffectCase>, !felt.type
          function.return %self : !struct.type<@ConstrainEffectCase>
        }
      }

      struct.def @UnmarkedEffectCase {
        struct.member @out : !felt.type

        function.def @product(%value: !felt.type) -> !struct.type<@UnmarkedEffectCase> {
          %self = struct.new : <@UnmarkedEffectCase>
          %c0 = arith.constant 0 : index
          %c1 = arith.constant 1 : index
          %c2 = arith.constant 2 : index
          %one = felt.const 1

          scf.for %i = %c0 to %c2 step %c1 {
            %computed = felt.add %value, %one
            scf.yield
          } {product_source = "compute"}

          %stored = felt.const 4
          global.write @g = %stored : !felt.type

          scf.for %i = %c0 to %c2 step %c1 {
            %actual = global.read @g : !felt.type {product_source = "constrain"}
            constrain.eq %actual, %value : !felt.type
            scf.yield
          } {product_source = "constrain"}

          struct.writem %self[@out] = %value : <@UnmarkedEffectCase>, !felt.type
          function.return %self : !struct.type<@UnmarkedEffectCase>
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

  unsigned computeLoops = 0;
  unsigned constrainLoops = 0;
  unsigned fusedLoops = 0;
  module->walk([&](mlir::scf::ForOp loop) {
    mlir::StringAttr source = loop->getAttrOfType<mlir::StringAttr>("product_source");
    if (!source) {
      return;
    }
    if (source.getValue() == "compute") {
      ++computeLoops;
    } else if (source.getValue() == "constrain") {
      ++constrainLoops;
    } else if (source.getValue() == "fused") {
      ++fusedLoops;
    }
  });

  EXPECT_EQ(computeLoops, 4U);
  EXPECT_EQ(constrainLoops, 4U);
  EXPECT_EQ(fusedLoops, 0U);
}

TEST_F(FuseProductControlFlowTests, MemberWriteSinkDoesNotCrossConstrainRead) {
  // A member write may be sunk only when the target loop cannot observe component state. A read
  // inside the constrain loop would see the write before fusion but after it in the fused order.
  mlir::OwningOpRef<mlir::ModuleOp> module = mlir::parseSourceString<mlir::ModuleOp>(
      R"mlir(
    module attributes {llzk.lang = "llzk"} {
      struct.def @A {
        struct.member @value : !felt.type

        function.def @product() -> !struct.type<@A> {
          %self = struct.new : <@A>
          %c0 = arith.constant 0 : index
          %c1 = arith.constant 1 : index
          %c2 = arith.constant 2 : index
          %value = felt.const 7

          scf.for %i = %c0 to %c2 step %c1 {
            %computed = felt.add %value, %value
            scf.yield
          } {product_source = "compute"}

          struct.writem %self[@value] = %value : <@A>, !felt.type {
            product_source = "compute"
          }

          scf.for %i = %c0 to %c2 step %c1 {
            %observed = struct.readm %self[@value] : <@A>, !felt.type {
              product_source = "constrain"
            }
            constrain.eq %observed, %value : !felt.type, !felt.type
            scf.yield
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

  llzk::component::MemberWriteOp write;
  llzk::component::MemberReadOp read;
  mlir::scf::ForOp constrainLoop;
  unsigned fusedLoops = 0;
  product.walk([&](llzk::component::MemberWriteOp writeOp) { write = writeOp; });
  product.walk([&](llzk::component::MemberReadOp readOp) { read = readOp; });
  product.walk([&](mlir::scf::ForOp loop) {
    mlir::StringAttr source = loop->getAttrOfType<mlir::StringAttr>("product_source");
    if (!source) {
      return;
    }
    if (source.getValue() == "constrain") {
      constrainLoop = loop;
    } else if (source.getValue() == "fused") {
      ++fusedLoops;
    }
  });

  ASSERT_TRUE(write);
  ASSERT_TRUE(read);
  ASSERT_TRUE(constrainLoop);
  EXPECT_EQ(fusedLoops, 0U);
  EXPECT_TRUE(write->isBeforeInBlock(constrainLoop));
}

TEST_F(FuseProductControlFlowTests, MemberWriteSinkRemainsFusibleWithoutTargetRead) {
  // Direct member writes retain their existing sink behavior when the constrain loop has no
  // storage or unknown effect that could observe the write.
  mlir::OwningOpRef<mlir::ModuleOp> module = mlir::parseSourceString<mlir::ModuleOp>(
      R"mlir(
    module attributes {llzk.lang = "llzk"} {
      struct.def @A {
        struct.member @value : !felt.type

        function.def @product(%input: !felt.type) -> !struct.type<@A> {
          %self = struct.new : <@A>
          %c0 = arith.constant 0 : index
          %c1 = arith.constant 1 : index
          %c2 = arith.constant 2 : index

          scf.for %i = %c0 to %c2 step %c1 {
            %computed = felt.add %input, %input
            scf.yield
          } {product_source = "compute"}

          struct.writem %self[@value] = %input : <@A>, !felt.type {
            product_source = "compute"
          }

          scf.for %i = %c0 to %c2 step %c1 {
            %expected = felt.const 0
            constrain.eq %input, %expected : !felt.type, !felt.type
            scf.yield
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

  llzk::component::MemberWriteOp write;
  mlir::scf::ForOp fusedLoop;
  product.walk([&](llzk::component::MemberWriteOp writeOp) { write = writeOp; });
  product.walk([&](mlir::scf::ForOp loop) {
    mlir::StringAttr source = loop->getAttrOfType<mlir::StringAttr>("product_source");
    if (source && source.getValue() == "fused") {
      fusedLoop = loop;
    }
  });

  ASSERT_TRUE(write);
  ASSERT_TRUE(fusedLoop);
  EXPECT_TRUE(fusedLoop->isBeforeInBlock(write));
}

} // namespace
