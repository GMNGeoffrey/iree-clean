// Copyright 2020 Google LLC
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

#include "iree/compiler/Dialect/HAL/Target/SPIRVCommon/SPIRVTarget.h"

#include "iree/compiler/Conversion/LinalgToSPIRV/Attributes.h"
#include "iree/compiler/Dialect/Flow/IR/FlowOps.h"
#include "iree/compiler/Dialect/HAL/Target/TargetRegistry.h"
#include "iree/compiler/Dialect/Shape/IR/ShapeOps.h"
#include "mlir/Dialect/SPIRV/Serialization.h"
#include "mlir/Dialect/SPIRV/TargetAndABI.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Matchers.h"

namespace mlir {
namespace iree_compiler {
namespace IREE {
namespace HAL {

// Records a full execution barrier that forces visibility of all buffers.
static void recordFullExecutionBarrier(Value commandBuffer, Location loc,
                                       OpBuilder &builder) {
  Value memoryBarrier = builder.create<IREE::HAL::MakeMemoryBarrierOp>(
      loc, IREE::HAL::AccessScopeBitfield::DispatchWrite,
      IREE::HAL::AccessScopeBitfield::DispatchRead);
  builder.create<IREE::HAL::CommandBufferExecutionBarrierOp>(
      loc, commandBuffer, IREE::HAL::ExecutionStageBitfield::Dispatch,
      IREE::HAL::ExecutionStageBitfield::Dispatch,
      ArrayRef<Value>{memoryBarrier}, ArrayRef<Value>{});
}

/// The codegeneration emits a function `numWorkgroupsFn` for each entry point
/// function. This function has arguments the !shapex.ranked_shape for all the
/// input and output shaped types. Using this the function returns the number of
/// workgroups to use. To use this function on the host side, generate the
/// !shapex.ranked_shape values that describe the shape of the inputs and
/// outputs of the dispatch region and "inline" the function body.
static std::array<Value, 3> calculateWorkgroupCountFromNumWorkgroupsFn(
    Location loc, FuncOp numWorkgroupsFn, IREE::HAL::InterfaceOp interface,
    ArrayRef<Optional<TensorRewriteAdaptor>> operands,
    ArrayRef<Optional<TensorRewriteAdaptor>> results,
    ConversionPatternRewriter &rewriter) {
  std::array<Value, 3> returnValue = {nullptr, nullptr, nullptr};
  // TODO: This is really just inlining a function. For now assume that the
  // `numWorkgroupsFn` has a single block to make inlining easier.
  if (!numWorkgroupsFn || !llvm::hasSingleElement(numWorkgroupsFn))
    return returnValue;
  SmallVector<SmallVector<Value, 4>, 4> shapeValues;
  shapeValues.reserve(operands.size() + results.size());
  auto getShapeValuesFn =
      [&](ArrayRef<Optional<TensorRewriteAdaptor>> values) -> LogicalResult {
    for (auto val : values) {
      if (!val) continue;
      Optional<SmallVector<Value, 4>> shape = val->getShapeDims(rewriter);
      if (!shape) return emitError(loc, "shape computation for operand failed");
      shapeValues.push_back(shape.getValue());
    }
    return success();
  };
  if (failed(getShapeValuesFn(operands)) || failed(getShapeValuesFn(results)))
    return returnValue;
  BlockAndValueMapping mapper;
  for (Operation &op : numWorkgroupsFn.front()) {
    if (isa<mlir::ReturnOp>(op)) {
      for (unsigned i = 0, e = std::min<unsigned>(3, op.getNumOperands());
           i != e; ++i) {
        returnValue[i] = mapper.lookupOrNull(op.getOperand(i));
      }
      break;
    }
    if (auto shapeOp = dyn_cast<Shape::RankedDimOp>(op)) {
      if (BlockArgument arg = shapeOp.shape().dyn_cast<BlockArgument>()) {
        auto &dimValues = shapeValues[arg.getArgNumber()];
        mapper.map(shapeOp.result(), dimValues[shapeOp.getIndex()]);
        continue;
      }
      return returnValue;
    }
    // If all its operands are mapped, clone it.
    if (llvm::all_of(op.getOperands(), [&mapper](Value operand) {
          return mapper.contains(operand);
        })) {
      rewriter.clone(op, mapper);
      continue;
    }
  }
  return returnValue;
}

SPIRVTargetBackend::SPIRVTargetBackend(SPIRVCodegenOptions options)
    : spvCodeGenOptions_(std::move(options)) {}

void SPIRVTargetBackend::declareTargetOpsForEnv(
    IREE::Flow::ExecutableOp sourceOp, IREE::HAL::ExecutableOp executableOp,
    spirv::TargetEnvAttr spvTargetEnv) {
  auto targetBuilder = OpBuilder::atBlockTerminator(&executableOp.getBlock());
  auto targetOp = targetBuilder.create<IREE::HAL::ExecutableTargetOp>(
      sourceOp.getLoc(), name(), filter_pattern());

  auto containerBuilder = OpBuilder::atBlockTerminator(&targetOp.getBlock());
  auto innerModuleOp = containerBuilder.create<ModuleOp>(sourceOp.getLoc());

  // Attach SPIR-V target environment to the target's ModuleOp.
  // If we had multiple target environments we would generate one target op
  // per environment, with each setting its own environment attribute.
  innerModuleOp.setAttr(spirv::getTargetEnvAttrName(), spvTargetEnv);
}

void SPIRVTargetBackend::buildTranslationPassPipeline(
    IREE::HAL::ExecutableTargetOp targetOp, OpPassManager &passManager) {
  buildSPIRVTransformPassPipeline(passManager, spvCodeGenOptions_);
}

LogicalResult SPIRVTargetBackend::recordDispatch(
    Location loc, DispatchState dispatchState,
    DeviceSwitchBuilder &switchBuilder) {
  // Multiple entry points might be generated for a single dispatch function.
  // Under such circumstances, we will have a special attribute indicating the
  // schedule of the split entry points. Try to see if we can find such
  // schedule attribute first.
  ArrayAttr entryPointScheduleAttr;
  spirv::ModuleOp spvModuleOp;
  IREE::HAL::ExecutableOp executableOp = dispatchState.executableOp;
  for (auto executableTargetOp :
       executableOp.getBlock().getOps<IREE::HAL::ExecutableTargetOp>()) {
    if (matchPattern(executableTargetOp.target_backend_filter(),
                     filter_pattern())) {
      ModuleOp innerModuleOp = executableTargetOp.getInnerModule();
      auto spvModuleOps = innerModuleOp.getOps<spirv::ModuleOp>();
      assert(llvm::hasSingleElement(spvModuleOps));
      spvModuleOp = *spvModuleOps.begin();
      entryPointScheduleAttr = innerModuleOp.getAttrOfType<ArrayAttr>(
          iree_compiler::getEntryPointScheduleAttrName());
      break;
    }
  }
  if (!spvModuleOp) return executableOp.emitError("unable to find spv.module");

  SmallVector<spirv::FuncOp, 2> spvEntryPointFns;
  if (!entryPointScheduleAttr) {
    for (spirv::FuncOp spvFuncOp : spvModuleOp.getOps<spirv::FuncOp>()) {
      if (SymbolTable::getSymbolVisibility(spvFuncOp) ==
          SymbolTable::Visibility::Public)
        spvEntryPointFns.push_back(spvFuncOp);
    }
    if (!llvm::hasSingleElement(spvEntryPointFns)) {
      return spvModuleOp.emitError(
                 "expected a single entry point function, found ")
             << spvEntryPointFns.size();
    }
  } else {
    llvm::StringMap<spirv::FuncOp> publicFns;
    for (spirv::FuncOp spvFuncOp : spvModuleOp.getOps<spirv::FuncOp>()) {
      if (SymbolTable::getSymbolVisibility(spvFuncOp) ==
          SymbolTable::Visibility::Public)
        publicFns[spvFuncOp.sym_name()] = spvFuncOp;
    }
    for (Attribute entryNameAttr : entryPointScheduleAttr) {
      StringRef entryName = entryNameAttr.cast<StringAttr>().getValue();
      spirv::FuncOp spvFuncOp = publicFns.lookup(entryName);
      if (!spvFuncOp)
        return spvModuleOp.emitError("unable to find entry point function ")
               << entryName;
      spvEntryPointFns.push_back(spvFuncOp);
    }
  }

  auto *region = switchBuilder.addConditionRegion(
      IREE::HAL::DeviceMatchIDAttr::get(filter_pattern(), loc.getContext()),
      {
          dispatchState.workload,
          dispatchState.commandBuffer,
          dispatchState.executable,
      });

  auto &entryBlock = region->front();
  ConversionPatternRewriter &rewriter = switchBuilder.getRewriter();
  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPointToEnd(&entryBlock);
  auto workload = entryBlock.getArgument(0);
  auto commandBuffer = entryBlock.getArgument(1);
  auto executable = entryBlock.getArgument(2);

  // We have multiple entry points to dispatch. Record in the order
  // specified by entry point schedule and insert barrier between sequential
  // ones.
  for (auto it : llvm::enumerate(spvEntryPointFns)) {
    spirv::FuncOp spvFuncOp = it.value();
    auto workgroupSize = calculateDispatchWorkgroupSize(
        loc, spvModuleOp, spvFuncOp.sym_name(), workload, rewriter);

    FlatSymbolRefAttr numWorkgroupsFnAttr =
        spvFuncOp.getAttrOfType<FlatSymbolRefAttr>(
            getNumWorkgroupsFnAttrName());

    std::array<Value, 3> workgroupCount = {nullptr, nullptr, nullptr};
    if (numWorkgroupsFnAttr) {
      FuncOp numWorkgroupsFn = dyn_cast<FuncOp>(SymbolTable::lookupSymbolIn(
          spvFuncOp.getParentOfType<ModuleOp>(), numWorkgroupsFnAttr));
      if (!numWorkgroupsFn) return failure();
      workgroupCount = calculateWorkgroupCountFromNumWorkgroupsFn(
          loc, numWorkgroupsFn, executableOp.getInterfaceOp(),
          dispatchState.operands, dispatchState.results, rewriter);
    } else {
      workgroupCount = calculateDispatchWorkgroupCount(loc, workload,
                                                       workgroupSize, rewriter);
    }

    if (llvm::any_of(workgroupCount,
                     [](Value v) -> bool { return v == nullptr; }))
      return spvFuncOp.emitError("unable to find workgroup count");

    // Ordinals are fixed based on the precomputed schedule, so use
    // CommandBufferDispatchOp instead of CommandBufferDispatchSymbolOp.
    int32_t entryPointOrdinal = it.index();
    rewriter.create<IREE::HAL::CommandBufferDispatchOp>(
        loc, commandBuffer, executable,
        rewriter.getI32IntegerAttr(entryPointOrdinal), workgroupCount[0],
        workgroupCount[1], workgroupCount[2]);
    if (it.index() + 1 != spvEntryPointFns.size()) {
      recordFullExecutionBarrier(commandBuffer, loc, rewriter);
    }
  }

  rewriter.create<IREE::HAL::ReturnOp>(loc);
  return success();
}

// Finds the spv.ExecutionMode operation to get the workgroup size from.
// TODO(ravishankarm): This might not be the only way this is specified. You
// could also have a spec constant, but that is not generated in the
// spv.module right now.
// TODO(ravishankarm): change workgroup size calculation to something we can
// query independently so that we don't need to lookup the value here.
std::array<Value, 3> SPIRVTargetBackend::calculateDispatchWorkgroupSize(
    Location loc, IREE::HAL::ExecutableOp executableOp,
    IREE::HAL::ExecutableEntryPointOp entryPointOp, Value workload,
    OpBuilder &builder) {
  // TODO(ravishankarm): possibly emit different recordDispatch logic if the
  // workgroup sizes differ among targets.
  spirv::ModuleOp spvModuleOp;
  for (auto executableTargetOp :
       executableOp.getBlock().getOps<IREE::HAL::ExecutableTargetOp>()) {
    if (matchPattern(executableTargetOp.target_backend_filter(),
                     filter_pattern())) {
      ModuleOp innerModuleOp = executableTargetOp.getInnerModule();
      assert(!innerModuleOp.getAttr(
          iree_compiler::getEntryPointScheduleAttrName()));
      auto spvModuleOps = innerModuleOp.getOps<spirv::ModuleOp>();
      assert(llvm::hasSingleElement(spvModuleOps));
      spvModuleOp = *spvModuleOps.begin();
      break;
    }
  }
  return calculateDispatchWorkgroupSize(
      loc, spvModuleOp, entryPointOp.sym_name(), workload, builder);
}

std::array<Value, 3> SPIRVTargetBackend::calculateDispatchWorkgroupSize(
    Location loc, spirv::ModuleOp spvModuleOp, StringRef entryPointName,
    Value workload, OpBuilder &builder) {
  std::array<Value, 3> workgroupSize;
  for (auto executionModeOp :
       spvModuleOp.getBlock().getOps<spirv::ExecutionModeOp>()) {
    if (executionModeOp.fn() == entryPointName &&
        executionModeOp.execution_mode() == spirv::ExecutionMode::LocalSize) {
      for (int i = 0; i < executionModeOp.values().size(); ++i) {
        workgroupSize[i] =
            builder.create<ConstantIndexOp>(loc, executionModeOp.values()[i]
                                                     .cast<IntegerAttr>()
                                                     .getValue()
                                                     .getZExtValue());
      }
      break;
    }
  }

  // Pad out the workgroup size with 1's (if the original rank was < 3).
  for (int i = 0; i < workgroupSize.size(); ++i) {
    if (!workgroupSize[i]) {
      workgroupSize[i] = builder.create<ConstantIndexOp>(loc, 1);
    }
  }

  return workgroupSize;
}

}  // namespace HAL
}  // namespace IREE
}  // namespace iree_compiler
}  // namespace mlir
