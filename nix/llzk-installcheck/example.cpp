#include "llzk/Dialect/LLZK/IR/Dialect.h"

#include <mlir/IR/MLIRContext.h>

int main() {
  mlir::MLIRContext context;
  context.loadDialect<llzk::LLZKDialect>();
  return 0;
}
