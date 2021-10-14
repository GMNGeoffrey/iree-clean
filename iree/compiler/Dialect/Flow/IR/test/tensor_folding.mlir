// Tests folding and canonicalization of tensor ops.

// RUN: iree-opt -split-input-file -canonicalize %s | iree-opt -split-input-file | IreeFileCheck %s

// CHECK-LABEL: @reshapeNoOpScalar
func @reshapeNoOpScalar(%arg0: tensor<f32>) -> tensor<f32> {
  // CHECK-NEXT: return %arg0 : tensor<f32>
  %0 = flow.tensor.reshape %arg0 : tensor<f32> -> tensor<f32>
  return %0 : tensor<f32>
}

// -----

// CHECK-LABEL: @reshapeNoOpStatic
func @reshapeNoOpStatic(%arg0: tensor<4x4xf32>) -> tensor<4x4xf32> {
  // CHECK-NEXT: return %arg0 : tensor<4x4xf32>
  %0 = flow.tensor.reshape %arg0 : tensor<4x4xf32> -> tensor<4x4xf32>
  return %0 : tensor<4x4xf32>
}

// -----

// CHECK-LABEL: @reshapeRankDifferent
func @reshapeRankDifferent(%arg0: tensor<1xf32>) -> tensor<f32> {
  // CHECK-NEXT: flow.tensor.reshape %arg0
  %0 = flow.tensor.reshape %arg0 : tensor<1xf32> -> tensor<f32>
  return %0 : tensor<f32>
}

// -----

// CHECK-LABEL: @reshapeStaticDifferent
func @reshapeStaticDifferent(%arg0: tensor<1x4xf32>) -> tensor<4x1xf32> {
  // CHECK-NEXT: flow.tensor.reshape %arg0
  %0 = flow.tensor.reshape %arg0 : tensor<1x4xf32> -> tensor<4x1xf32>
  return %0 : tensor<4x1xf32>
}

// -----

// CHECK-LABEL: @reshapeNoOpDynamic
func @reshapeNoOpDynamic(%arg0: tensor<4x?xf32>, %dim: index) -> tensor<4x?xf32> {
  // CHECK-NEXT: return %arg0 : tensor<4x?xf32>
  %0 = flow.tensor.reshape %arg0 : tensor<4x?xf32>{%dim} -> tensor<4x?xf32>{%dim}
  return %0 : tensor<4x?xf32>
}

// -----

// CHECK-LABEL: @reshapeDynamicDifferent
func @reshapeDynamicDifferent(%arg0: tensor<4x?xf32>, %dim0: index, %dim1: index) -> tensor<4x?xf32> {
  // CHECK-NEXT: flow.tensor.reshape %arg0
  %0 = flow.tensor.reshape %arg0 : tensor<4x?xf32>{%dim0} -> tensor<4x?xf32>{%dim1}
  return %0 : tensor<4x?xf32>
}

// -----

// CHECK-LABEL: @flattenReshapeChain
// CHECK-SAME: %[[ARG:.+]]: tensor<4x?xf32>,
// CHECK-SAME: %[[DIM0:.+]]: index, %[[DIM1:.+]]: index, %[[DIM2:.+]]: index
func @flattenReshapeChain(%arg0: tensor<4x?xf32>, %dim0: index, %dim1: index, %dim2: index) -> tensor<4x?xf32> {
  // CHECK-NEXT: %[[RET:.+]] = flow.tensor.reshape %[[ARG]] : tensor<4x?xf32>{%[[DIM0]]} -> tensor<4x?xf32>{%[[DIM2]]}
  %0 = flow.tensor.reshape %arg0 : tensor<4x?xf32>{%dim0} -> tensor<4x?xf32>{%dim1}
  %1 = flow.tensor.reshape %0 : tensor<4x?xf32>{%dim1} -> tensor<4x?xf32>{%dim2}
  // CHECK-NEXT: return %[[RET]]
  return %1 : tensor<4x?xf32>
}

// -----

// CHECK-LABEL: @loadConst
func @loadConst() -> i32 {
  %0 = arith.constant dense<[[0, 1], [2, 3]]> : tensor<2x2xi32>
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  // CHECK-NEXT: %[[C2:.+]] = arith.constant 2 : i32
  %2 = flow.tensor.load %0[%c1, %c0] : tensor<2x2xi32>
  // CHECK-NEXT: return %[[C2]]
  return %2 : i32
}

// -----

// CHECK-LABEL: @loadConstScalar
func @loadConstScalar() -> i32 {
  %0 = arith.constant dense<4> : tensor<i32>
  // CHECK-NEXT: %[[C4:.+]] = arith.constant 4 : i32
  %1 = flow.tensor.load %0 : tensor<i32>
  // CHECK-NEXT: return %[[C4]]
  return %1 : i32
}

// -----

// CHECK-LABEL: @storeConst
func @storeConst() -> tensor<2x2xi32> {
  %0 = arith.constant dense<[[0, 1], [2, 3]]> : tensor<2x2xi32>
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %c4 = arith.constant 4 : i32
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<[
  // CHECK-SAME:     [0, 1], [4, 3]
  // CHECK-SAME: ]> : tensor<2x2xi32>
  %1 = flow.tensor.store %c4, %0[%c1, %c0] : tensor<2x2xi32>
  // CHECK-NEXT: return %[[C]]
  return %1 : tensor<2x2xi32>
}

// -----

// CHECK-LABEL: @storeConstScalar
func @storeConstScalar() -> tensor<i32> {
  %0 = arith.constant dense<0> : tensor<i32>
  %1 = arith.constant 4 : i32
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<4> : tensor<i32>
  %2 = flow.tensor.store %1, %0 : tensor<i32>
  // CHECK-NEXT: return %[[C]]
  return %2 : tensor<i32>
}

// -----

// CHECK-LABEL: @splatConst
func @splatConst() -> tensor<4xi32> {
  %0 = arith.constant 4 : i32
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<4> : tensor<4xi32>
  %1 = flow.tensor.splat %0 : tensor<4xi32>
  // CHECK-NEXT: return %[[C]]
  return %1 : tensor<4xi32>
}

// -----

// CHECK-LABEL: @splatConstScalar
func @splatConstScalar() -> tensor<i32> {
  %0 = arith.constant 4 : i32
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<4> : tensor<i32>
  %1 = flow.tensor.splat %0 : tensor<i32>
  // CHECK-NEXT: return %[[C]]
  return %1 : tensor<i32>
}

// -----

// CHECK-LABEL: @splatDynamicShape
//  CHECK-SAME: (%[[DIM0:.+]]: index, %[[DIM1:.+]]: index)
func @splatDynamicShape(%dim0: index, %dim1: index) -> tensor<?x?xi32> {
  // CHECK: %[[FOUR:.+]] = arith.constant 4 : i32
  %four = arith.constant 4 : i32
  // CHECK: %[[SPLAT:.+]] = flow.tensor.splat %[[FOUR]] : tensor<?x?xi32>{%[[DIM0]], %[[DIM1]]}
  %1 = flow.tensor.splat %four : tensor<?x?xi32>{%dim0, %dim1}
  // CHECK: return %[[SPLAT]]
  return %1 : tensor<?x?xi32>
}

// -----

// CHECK-LABEL: @cloneConst
func @cloneConst() -> tensor<4xi32> {
  %0 = arith.constant dense<[0, 1, 2, 3]> : tensor<4xi32>
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<[0, 1, 2, 3]> : tensor<4xi32>
  %1 = flow.tensor.clone %0 : tensor<4xi32>
  // CHECK-NEXT: return %[[C]]
  return %1 : tensor<4xi32>
}

// -----

// CHECK-LABEL: @sliceConst0D
func @sliceConst0D() -> tensor<i32> {
  %0 = arith.constant dense<0> : tensor<i32>
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<0> : tensor<i32>
  %1 = flow.tensor.slice %0[for] : tensor<i32> -> tensor<i32>
  // CHECK-NEXT: return %[[C]]
  return %1 : tensor<i32>
}

// -----

// CHECK-LABEL: @sliceConst1D
func @sliceConst1D() -> tensor<1xi32> {
  %0 = arith.constant dense<0> : tensor<1xi32>
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<0> : tensor<1xi32>
  %1 = flow.tensor.slice %0[%c0 for %c1] : tensor<1xi32> -> tensor<1xi32>
  // CHECK-NEXT: return %[[C]]
  return %1 : tensor<1xi32>
}

// -----

// CHECK-LABEL: @sliceConst1DZeroLength
func @sliceConst1DZeroLength() -> tensor<0xi32> {
  %0 = arith.constant dense<0> : tensor<1xi32>
  %c0 = arith.constant 0 : index
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<> : tensor<0xi32>
  %1 = flow.tensor.slice %0[%c0 for %c0] : tensor<1xi32> -> tensor<0xi32>
  // CHECK-NEXT: return %[[C]]
  return %1 : tensor<0xi32>
}

// -----

// CHECK-LABEL: @sliceConst2D
func @sliceConst2D() -> tensor<1x2xi32> {
  %0 = arith.constant dense<[[0, 1, 2], [3, 4, 5]]> : tensor<2x3xi32>
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %c2 = arith.constant 2 : index
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<[
  // CHECK-SAME:     [1, 2]
  // CHECK-SAME: ]> : tensor<1x2xi32>
  %1 = flow.tensor.slice %0[%c0, %c1 for %c1, %c2] : tensor<2x3xi32> -> tensor<1x2xi32>
  // CHECK-NEXT: return %[[C]]
  return %1 : tensor<1x2xi32>
}

// -----

// CHECK-LABEL: @sliceConst2DZeroLength1
func @sliceConst2DZeroLength1() -> tensor<1x0xi32> {
  %0 = arith.constant dense<[[0, 1, 2], [3, 4, 5]]> : tensor<2x3xi32>
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<> : tensor<1x0xi32>
  %1 = flow.tensor.slice %0[%c0, %c0 for %c1, %c0] : tensor<2x3xi32> -> tensor<1x0xi32>
  // CHECK-NEXT: return %[[C]]
  return %1 : tensor<1x0xi32>
}

// -----

// CHECK-LABEL: @sliceConst2DZeroLength01
func @sliceConst2DZeroLength01() -> tensor<0x0xi32> {
  %0 = arith.constant dense<[[0, 1, 2], [3, 4, 5]]> : tensor<2x3xi32>
  %c0 = arith.constant 0 : index
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<> : tensor<0x0xi32>
  %1 = flow.tensor.slice %0[%c0, %c0 for %c0, %c0] : tensor<2x3xi32> -> tensor<0x0xi32>
  // CHECK-NEXT: return %[[C]]
  return %1 : tensor<0x0xi32>
}

// -----

// CHECK-LABEL: @sliceConst3D
func @sliceConst3D() -> tensor<1x2x3xi32> {
  %0 = arith.constant dense<[[[0, 1, 2], [3, 4, 5], [6, 7, 8]], [[9, 10, 11], [12, 13, 14], [15, 16, 17]]]> : tensor<2x3x3xi32>
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %c2 = arith.constant 2 : index
  %c3 = arith.constant 3 : index
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<[
  // CHECK-SAME:                             [
  // CHECK-SAME:                              [3, 4, 5], [6, 7, 8]]]> : tensor<1x2x3xi32>
  %1 = flow.tensor.slice %0[%c0, %c1, %c0 for %c1, %c2, %c3] : tensor<2x3x3xi32> -> tensor<1x2x3xi32>
  // CHECK-NEXT: return %[[C]]
  return %1 : tensor<1x2x3xi32>
}

// -----

// CHECK-LABEL: @updateConst0D
func @updateConst0D() -> tensor<i32> {
  %0 = arith.constant dense<0> : tensor<i32>
  %1 = arith.constant dense<1> : tensor<i32>
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<0> : tensor<i32>
  %2 = flow.tensor.update %0, %1[] : tensor<i32> -> tensor<i32>
  // CHECK-NEXT: return %[[C]]
  return %2 : tensor<i32>
}

// -----

// CHECK-LABEL: @updateConst1D
func @updateConst1D() -> tensor<1xi32> {
  %0 = arith.constant dense<0> : tensor<1xi32>
  %1 = arith.constant dense<1> : tensor<1xi32>
  %c0 = arith.constant 0 : index
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<0> : tensor<1xi32>
  %2 = flow.tensor.update %0, %1[%c0] : tensor<1xi32> -> tensor<1xi32>
  // CHECK-NEXT: return %[[C]]
  return %2 : tensor<1xi32>
}

// -----

// CHECK-LABEL: @updateConst1DUpdateZeroSize
func @updateConst1DUpdateZeroSize() -> tensor<1xi32> {
  %0 = arith.constant dense<> : tensor<0xi32>
  %1 = arith.constant dense<1> : tensor<1xi32>
  %c0 = arith.constant 0 : index
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<1> : tensor<1xi32>
  %2 = flow.tensor.update %0, %1[%c0] : tensor<0xi32> -> tensor<1xi32>
  // CHECK-NEXT: return %[[C]]
  return %2 : tensor<1xi32>
}

// -----

// CHECK-LABEL: @updateConst2DUpdate1x1
func @updateConst2DUpdate1x1() -> tensor<3x4xi32> {
  %0 = arith.constant dense<[[12]]> : tensor<1x1xi32>
  %1 = arith.constant dense<[[0, 1, 2, 3], [4, 5, 6, 7], [8, 9, 10, 11]]> : tensor<3x4xi32>
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<[
  // CHECK-SAME: [0, 12, 2, 3], [4, 5, 6, 7], [8, 9, 10, 11]]> : tensor<3x4xi32>
  %2 = flow.tensor.update %0, %1[%c0, %c1] : tensor<1x1xi32> -> tensor<3x4xi32>
  // CHECK-NEXT: return %[[C]]
  return %2 : tensor<3x4xi32>
}

// -----

// CHECK-LABEL: @updateConst2DUpdate2x2
func @updateConst2DUpdate2x2() -> tensor<3x4xi32> {
  %0 = arith.constant dense<[[12, 13], [14, 15]]> : tensor<2x2xi32>
  %1 = arith.constant dense<[[0, 1, 2, 3], [4, 5, 6, 7], [8, 9, 10, 11]]> : tensor<3x4xi32>
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<[
  // CHECK-SAME: [0, 12, 13, 3], [4, 14, 15, 7], [8, 9, 10, 11]]> : tensor<3x4xi32>
  %2 = flow.tensor.update %0, %1[%c0, %c1] : tensor<2x2xi32> -> tensor<3x4xi32>
  // CHECK-NEXT: return %[[C]]
  return %2 : tensor<3x4xi32>
}

// -----

// CHECK-LABEL: @updateConst3DUpdate1x2x3
func @updateConst3DUpdate1x2x3() -> tensor<2x3x3xi32> {
  %0 = arith.constant dense<[[[18, 19, 20], [21, 22, 23]]]> : tensor<1x2x3xi32>
  %1 = arith.constant dense<[[[0, 1, 2], [3, 4, 5], [6, 7, 8]], [[9, 10, 11], [12, 13, 14], [15, 16, 17]]]> : tensor<2x3x3xi32>
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<[
  // CHECK-SAME:                             [
  // CHECK-SAME:                              [0, 1, 2], [18, 19, 20], [21, 22, 23]], [
  // CHECK-SAME: [9, 10, 11], [12, 13, 14], [15, 16, 17]]]> : tensor<2x3x3xi32>
  %2 = flow.tensor.update %0, %1[%c0, %c1, %c0] : tensor<1x2x3xi32> -> tensor<2x3x3xi32>
  // CHECK-NEXT: return %[[C]]
  return %2 : tensor<2x3x3xi32>
}

// -----

// CHECK-LABEL: @updateConst3DUpdate2x3x2
func @updateConst3DUpdate2x3x2() -> tensor<2x3x3xi32> {
  %0 = arith.constant dense<[[[18, 19], [20, 21], [22, 23]], [[24, 25], [26, 27], [28, 29]]]> : tensor<2x3x2xi32>
  %1 = arith.constant dense<[[[0, 1, 2], [3, 4, 5], [6, 7, 8]], [[9, 10, 11], [12, 13, 14], [15, 16, 17]]]> : tensor<2x3x3xi32>
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 0 : index
  // CHECK-NEXT: %[[C:.+]] = arith.constant dense<[
  // CHECK-SAME:                             [
  // CHECK-SAME:                              [18, 19, 2], [20, 21, 5], [22, 23, 8]], [
  // CHECK-SAME: [24, 25, 11], [26, 27, 14], [28, 29, 17]]]> : tensor<2x3x3xi32>
  %2 = flow.tensor.update %0, %1[%c0, %c1, %c0] : tensor<2x3x2xi32> -> tensor<2x3x3xi32>
  // CHECK-NEXT: return %[[C]]
  return %2 : tensor<2x3x3xi32>
}

// -----

// CHECK-LABEL: @updateReplace
func @updateReplace(%arg0 : tensor<4xi32>, %arg1 : tensor<4xi32>) -> tensor<4xi32> {
  %c0 = arith.constant 0 : index
  %0 = flow.tensor.update %arg0, %arg1[%c0] : tensor<4xi32> -> tensor<4xi32>
  // CHECK-NEXT: return %arg0
  return %0 : tensor<4xi32>
}

// -----

// CHECK-LABEL: @propogateStaticShapeOfTarget
func @propogateStaticShapeOfTarget(%arg0 : tensor<?x?xf32>, %arg1 : f32) -> tensor<?x?xf32> {
  %c21 = arith.constant 21 : index
  %c42 = arith.constant 42 : index
  %c2 = arith.constant 2 : index
  %c4 = arith.constant 4 : index
  // CHECK: %[[TARGET:.+]] = tensor.generate {
  // CHECK: } : tensor<21x42xf32>
  %0 = tensor.generate %c21, %c42 {
  ^bb0(%arg2: index, %arg3: index):
    tensor.yield %arg1 : f32
  } :  tensor<?x?xf32>
  // CHECK: %[[UPDATED:.+]] = flow.tensor.update %{{.+}}, %[[TARGET]]
  // CHECK: %[[RESULT:.+]] = tensor.cast %[[UPDATED]] : tensor<21x42xf32> to tensor<?x?xf32>
  %1 = flow.tensor.update %arg0, %0[%c2, %c4] : tensor<?x?xf32>{%c21, %c42} -> tensor<?x?xf32>{%c21, %c42}
  // CHECK: return %[[RESULT]]
  return %1 : tensor<?x?xf32>
}

// -----

// CHECK-LABEL: @propogateStaticShapeOfUpdate
func @propogateStaticShapeOfUpdate(%arg0 : tensor<?x?xf32>, %arg1 : f32) -> tensor<?x?xf32> {
  %c21 = arith.constant 21 : index
  %c42 = arith.constant 42 : index
  %c2 = arith.constant 2 : index
  %c4 = arith.constant 4 : index
  // CHECK: %[[UPDATE:.+]] = tensor.generate {
  // CHECK: } : tensor<21x42xf32>
  %0 = tensor.generate %c21, %c42 {
  ^bb0(%arg2: index, %arg3: index):
    tensor.yield %arg1 : f32
  } :  tensor<?x?xf32>
  // CHECK: %[[RESULT:.+]] = flow.tensor.update  %[[UPDATE]]
  %1 = flow.tensor.update %0, %arg0[%c2, %c4] : tensor<?x?xf32>{%c21, %c42} -> tensor<?x?xf32>{%c21, %c42}
  // CHECK: return %[[RESULT]]
  return %1 : tensor<?x?xf32>
}

// -----

// CHECK-LABEL: @foldSplatLoadIntoPrimitive
//  CHECK-SAME: (%[[arg0:.+]]: f32, %[[arg1:.+]]: index, %[[arg2:.+]]: index)
func @foldSplatLoadIntoPrimitive(%arg0 : f32, %arg1 : index, %arg2 : index) -> f32 {
  // CHECK-NEXT: return %[[arg0]] : f32
  %0 = flow.tensor.splat %arg0 : tensor<4x4xf32>
  %1 = flow.tensor.load %0[%arg1, %arg2] : tensor<4x4xf32>
  return %1 : f32
}

// -----

// CHECK-LABEL: @foldSplatReshapeIntoSplat
func @foldSplatReshapeIntoSplat(%arg0 : f32) -> tensor<16xf32> {
  // CHECK-NEXT: %0 = flow.tensor.splat %arg0 : tensor<16xf32>
  // CHECK-NEXT: return %0 : tensor<16xf32>
  %0 = flow.tensor.splat %arg0 : tensor<4x4xf32>
  %1 = flow.tensor.reshape %0 : tensor<4x4xf32> -> tensor<16xf32>
  return %1 : tensor<16xf32>
}

// CHECK-LABEL: @foldSplatReshapeIntoSplatDynamic
func @foldSplatReshapeIntoSplatDynamic(%arg0 : f32, %arg1 : index, %arg2 : index, %arg3 : index) -> tensor<?x?xf32> {
  // CHECK-NEXT: %0 = flow.tensor.splat %arg0 : tensor<?x?xf32>{%arg2, %arg3}
  // CHECK-NEXT: return %0 : tensor<?x?xf32>
  %0 = flow.tensor.splat %arg0 : tensor<?x4xf32>{%arg1}
  %1 = flow.tensor.reshape %0 : tensor<?x4xf32>{%arg1} -> tensor<?x?xf32>{%arg2, %arg3}
  return %1 : tensor<?x?xf32>
}
