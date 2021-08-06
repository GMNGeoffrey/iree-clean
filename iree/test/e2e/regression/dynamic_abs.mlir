// RUN: [[ $IREE_LLVMAOT_DISABLE == 1 ]] || (iree-run-mlir --iree-input-type=mhlo -iree-hal-target-backends=dylib-llvm-aot %s | IreeFileCheck %s)

// CHECK-LABEL: EXEC @dynamic_tensor
func @dynamic_tensor() -> tensor<?x?xf32> {
  %input = util.dynamic_shape_constant dense<[[-1.0, 2.0, -3.0], [4.0, -5.0, 6.0]]> : tensor<2x3xf32> -> tensor<?x?xf32>
  %res = "mhlo.abs"(%input) : (tensor<?x?xf32>) -> tensor<?x?xf32>
  return %res : tensor<?x?xf32>
}

// CHECK: 2x3xf32=[1 2 3][4 5 6]
