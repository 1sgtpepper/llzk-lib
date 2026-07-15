//===-- SMTAttributeTests.cpp - Unit tests for SMT attributes ---*- C++ -*-===//
//
// Part of the LLZK Project, under the Apache License v2.0.
// See LICENSE.txt for license information.
// Copyright 2026 Veridise Inc.
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//

#include "../LLZKTestBase.h"

#include "llzk/Dialect/SMT/IR/SMTAttributes.h"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/Hashing.h>

#include <cstdint>
#include <optional>

using namespace llzk::smt;

class SMTAttributeTests : public LLZKTest {};

namespace {

struct APIntStorageCollision {
  uint64_t value;
  unsigned narrowWidth;
  unsigned wideWidth;
  unsigned hash;
};

std::optional<APIntStorageCollision> findAPIntStorageCollision() {
  constexpr unsigned minWidth = 64;
  constexpr unsigned maxWidth = 4096;
  constexpr uint64_t maxValue = 100000;

  llvm::DenseMap<unsigned, unsigned> widthByHash;
  widthByHash.reserve(maxWidth - minWidth + 1);

  for (uint64_t value = 0; value < maxValue; ++value) {
    widthByHash.clear();
    for (unsigned width = minWidth; width <= maxWidth; ++width) {
      llvm::APInt key(width, value);
      unsigned hash = static_cast<unsigned>(static_cast<size_t>(llvm::hash_combine(key)));
      auto [iter, inserted] = widthByHash.try_emplace(hash, width);
      if (!inserted) {
        return APIntStorageCollision {value, iter->second, width, hash};
      }
    }
  }

  return std::nullopt;
}

} // namespace

TEST_F(SMTAttributeTests, BitVectorStorageDoesNotAliasCollidingWidths) {
  // Debug LLVM builds seed hashes per process, so fixed collision values are
  // not portable. Find a collision in-process to exercise storage equality.
  std::optional<APIntStorageCollision> collision = findAPIntStorageCollision();
  ASSERT_TRUE(collision.has_value()) << "failed to find an APInt storage hash collision";

  SCOPED_TRACE(
      ::testing::Message() << "value=" << collision->value
                           << " narrowWidth=" << collision->narrowWidth
                           << " wideWidth=" << collision->wideWidth << " hash=" << collision->hash
  );

  llvm::APInt narrowValue(collision->narrowWidth, collision->value);
  llvm::APInt wideValue(collision->wideWidth, collision->value);
  ASSERT_TRUE(llvm::APInt::isSameValue(narrowValue, wideValue));
  ASSERT_EQ(
      static_cast<unsigned>(static_cast<size_t>(llvm::hash_combine(narrowValue))), collision->hash
  );
  ASSERT_EQ(
      static_cast<unsigned>(static_cast<size_t>(llvm::hash_combine(wideValue))), collision->hash
  );

  BitVectorAttr narrow = BitVectorAttr::get(&ctx, collision->value, collision->narrowWidth);
  BitVectorAttr wide = BitVectorAttr::get(&ctx, collision->value, collision->wideWidth);

  EXPECT_NE(narrow, wide);
  EXPECT_EQ(narrow.getValue().getBitWidth(), collision->narrowWidth);
  EXPECT_EQ(wide.getValue().getBitWidth(), collision->wideWidth);
}
