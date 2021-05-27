// Copyright 2020 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Dialect/Flow/IR/FlowDialect.h"
#include "iree/compiler/Dialect/Flow/IR/FlowOps.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/PatternMatch.h"

namespace mlir {
namespace iree_compiler {
namespace IREE {
namespace Flow {

//------------------------------------------------------------------------------
// Closure optimization
//------------------------------------------------------------------------------

// Modifies in-place the operand results vectors for a closure operation.
// |excludedOperandIndices| and |excludedResultIndices| are sets containing the
// operands and results in the lists to remove.
void excludeClosureOperandsAndResults(SmallVector<Value, 4> &operandValues,
                                      ArrayRef<unsigned> excludedOperandIndices,
                                      SmallVector<Type, 4> &resultTypes,
                                      ArrayRef<unsigned> excludedResultIndices);
void excludeClosureOperandsAndResults(SmallVector<Value, 4> &operandValues,
                                      SmallVector<Value, 4> &operandDims,
                                      ArrayRef<unsigned> excludedOperandIndices,
                                      SmallVector<Type, 4> &resultTypes,
                                      SmallVector<Value, 4> &resultDims,
                                      ArrayRef<unsigned> excludedResultIndices);

// Erases the given result indices from terminators in the given region.
void eraseRegionResults(Region &region,
                        ArrayRef<unsigned> excludedResultIndices);

// Optimizes closure |closureOp| to remove duplicate operands and unused
// results. The op may be mutated, destroyed, or replaced with a new one. If an
// optional |rewriter| is provided then it will be notified of the operations
// performed on the op. Returns true if the op was optimized.
LogicalResult optimizeClosureLikeOp(ClosureOpInterface closureOp,
                                    PatternRewriter &rewriter);

// A pattern that optimizes the given region-containing op T (CSE, DCE, etc).
// Duplicate operands will be combined and unused operands and results will be
// removed.
//
// T must implement the IREE::Flow::ClosureOpInterface.
template <typename T>
struct ClosureOptimizationPattern : public OpRewritePattern<T> {
  using OpRewritePattern<T>::OpRewritePattern;

  LogicalResult matchAndRewrite(T op,
                                PatternRewriter &rewriter) const override {
    auto closureOp = cast<ClosureOpInterface>(op.getOperation());
    return optimizeClosureLikeOp(closureOp, rewriter);
  }
};

}  // namespace Flow
}  // namespace IREE
}  // namespace iree_compiler
}  // namespace mlir
