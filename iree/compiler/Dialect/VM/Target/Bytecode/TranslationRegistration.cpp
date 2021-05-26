// Copyright 2019 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Dialect/VM/Target/Bytecode/BytecodeModuleTarget.h"
#include "iree/compiler/Dialect/VM/Target/Bytecode/TranslationFlags.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Visitors.h"
#include "mlir/Translation.h"

namespace mlir {
namespace iree_compiler {
namespace IREE {
namespace VM {

void registerToVMBytecodeTranslation() {
  TranslateFromMLIRRegistration toBytecodeModule(
      "iree-vm-ir-to-bytecode-module",
      [](mlir::ModuleOp moduleOp, llvm::raw_ostream &output) {
        return translateModuleToBytecode(
            moduleOp, getBytecodeTargetOptionsFromFlags(), output);
      });
}

}  // namespace VM
}  // namespace IREE
}  // namespace iree_compiler
}  // namespace mlir
