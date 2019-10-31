// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "iree/compiler/IR/Interpreter/HLDialect.h"

#include "iree/compiler/IR/Interpreter/HLOps.h"
#include "third_party/llvm/llvm/include/llvm/Support/SourceMgr.h"

namespace mlir {
namespace iree_compiler {

IREEHLInterpreterDialect::IREEHLInterpreterDialect(MLIRContext* context)
    : Dialect(getDialectNamespace(), context) {
#define GET_OP_LIST
  addOperations<
#include "iree/compiler/IR/Interpreter/HLOps.cpp.inc"
      >();
}

static DialectRegistration<IREEHLInterpreterDialect> iree_hl_interp_dialect;

}  // namespace iree_compiler
}  // namespace mlir
