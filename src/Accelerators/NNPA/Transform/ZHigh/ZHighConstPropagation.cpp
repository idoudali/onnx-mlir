/*
 * SPDX-License-Identifier: Apache-2.0
 */

//===-------- ZHighConstPropagation.cpp - ZHigh High Level Optimizer ------===//
//
// Copyright 2019 The IBM Research Authors.
//
// =============================================================================
//
// This file implements a set of simple combiners for optimizing operations in
// the ZHigh dialect.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/AsmState.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#include "src/Accelerators/NNPA/Dialect/ZHigh/ZHighOps.hpp"
#include "src/Accelerators/NNPA/Dialect/ZHigh/ZHighOps/OpHelper.hpp"
#include "src/Accelerators/NNPA/Pass/NNPAPasses.hpp"
#include "src/Accelerators/NNPA/Support/LayoutHelper.hpp"
#include "src/Accelerators/NNPA/Support/Stickify/Stickify.hpp"
#include "src/Compiler/CompilerOptions.hpp"
#include "src/Dialect/ONNX/ONNXOps.hpp"
#include "src/Dialect/ONNX/ONNXOps/OpHelper.hpp"

using namespace mlir;
using namespace onnx_mlir;
using namespace onnx_mlir::zhigh;

namespace onnx_mlir {
namespace zhigh {

/// Emit a ZHighStikifiedConstant using information from a stickified ztensor.
ZHighStickifiedConstantOp emitZHighStickifiedConstant(PatternRewriter &rewriter,
    Location loc, zdnn_ztensor *ztensor, Type outputType) {

  // Create a ZHighStickifiedConstantOp.
  ZHighStickifiedConstantOp stickifiedConstant =
      rewriter.create<ZHighStickifiedConstantOp>(loc, outputType,
          /*stickified=*/rewriter.getBoolAttr(true),
          /*value=*/nullptr,
          /*alignment=*/rewriter.getI64IntegerAttr(4096));

  // Use an dense resource attribute to store stickified data.
  // Attribute type: tensor<sizeInBytes x i8>
  int64_t sizeInBytes = ztensor->buffer_size;
  DenseResourceElementsAttr valueAttr = DenseUI8ResourceElementsAttr::get(
      RankedTensorType::get({sizeInBytes}, rewriter.getI8Type()),
      stickifiedConstant.getOperation()
          ->getDialect()
          ->getNamespace(), // use the dialect as the blob "hint"
      HeapAsmResourceBlob::allocateAndCopyWithAlign(
          llvm::ArrayRef((char *)ztensor->buffer, sizeInBytes), alignof(char)));

  stickifiedConstant.setValueAttr(valueAttr);

  return stickifiedConstant;
}

ZHighStickifiedConstantOp createConstantForStick(PatternRewriter &rewriter,
    Value replacingValue, Value input, StringAttr layout) {
  Location loc = replacingValue.getLoc();
  Operation *op = input.getDefiningOp();
  // Read dense attributes.
  DenseElementsAttr dataAttr = mlir::dyn_cast_or_null<mlir::DenseElementsAttr>(
      op->getAttrOfType<::mlir::Attribute>("value"));
  assert(dataAttr && "Attribute is null");
  // Keep previous implementation about generating stickified data at
  // ZHighConstPropagationPass. To use this, comment in and set directive "
  // NNPA_ZHIGH_STICKIFIEDCONST_GEN"
  //
  // #ifdef NNPA_ZHIGH_STICKIFIEDCONST_GEN
  //   // Set stickified data.
  //   ArrayRef<char> stickifiedData =
  //       getStickifiedDataOfDenseElemAttr(dataAttr, layout);
  //   // Create a ZHighStickifiedConstantOp.
  //   ZHighStickifiedConstantOp constantOp =
  //       rewriter.create<ZHighStickifiedConstantOp>(loc,
  //       replacingValue.getType(),
  //           /*stickified=*/rewriter.getBoolAttr(true),
  //           /*value=*/nullptr,
  //           /*alignment=*/rewriter.getI64IntegerAttr(4096));
  //
  //   // Use an dense resource attribute to store stickified data.
  //   // Attribute type: tensor<sizeInBytes x i8>
  //   DenseResourceElementsAttr valueAttr = DenseUI8ResourceElementsAttr::get(
  //       RankedTensorType::get({stickifiedData.size()}, rewriter.getI8Type()),
  //       constantOp.getOperation()
  //           ->getDialect()
  //           ->getNamespace(), // use the dialect as the blob "hint"
  //       HeapAsmResourceBlob::allocateAndCopyWithAlign(
  //           stickifiedData, alignof(char)));
  //
  //   constantOp.setValueAttr(valueAttr);
  // #else
  ZHighStickifiedConstantOp constantOp =
      rewriter.create<ZHighStickifiedConstantOp>(loc, replacingValue.getType(),
          /*stickified=*/rewriter.getBoolAttr(false),
          /*value=*/dataAttr,
          /*alignment=*/rewriter.getI64IntegerAttr(4096));
  // #endif //  NNPA_ZHIGH_STICKIFIEDCONST_GEN
  return constantOp;
}

ZHighStickifiedConstantOp createConstantForStickForLSTM(
    PatternRewriter &rewriter, Value replacingValue, Value inputF, Value inputI,
    Value inputC, Value inputO) {
  Location loc = replacingValue.getLoc();
  Operation *fOp = inputF.getDefiningOp();
  Operation *iOp = inputI.getDefiningOp();
  Operation *cOp = inputC.getDefiningOp();
  Operation *oOp = inputO.getDefiningOp();

  ArrayRef<int64_t> fShape =
      mlir::cast<ShapedType>(inputF.getType()).getShape();
  assert((fShape.size() == 2 || fShape.size() == 3) && "Wrong tensor shape");
  Type elementType = mlir::cast<ShapedType>(inputF.getType()).getElementType();

  // Read dense attributes.
  DenseElementsAttr fDataAttr = mlir::dyn_cast_or_null<mlir::DenseElementsAttr>(
      fOp->getAttrOfType<::mlir::Attribute>("value"));
  DenseElementsAttr iDataAttr = mlir::dyn_cast_or_null<mlir::DenseElementsAttr>(
      iOp->getAttrOfType<::mlir::Attribute>("value"));
  DenseElementsAttr cDataAttr = mlir::dyn_cast_or_null<mlir::DenseElementsAttr>(
      cOp->getAttrOfType<::mlir::Attribute>("value"));
  DenseElementsAttr oDataAttr = mlir::dyn_cast_or_null<mlir::DenseElementsAttr>(
      oOp->getAttrOfType<::mlir::Attribute>("value"));
  assert((fDataAttr && iDataAttr && cDataAttr && oDataAttr) &&
         "Attribute is null");
  // Read attributes's raw data.
  std::vector<char> rawFData, rawIData, rawCData, rawOData;
  getRawData(fDataAttr, rawFData);
  getRawData(iDataAttr, rawIData);
  getRawData(cDataAttr, rawCData);
  getRawData(oDataAttr, rawOData);

  // Call stickify.
  zdnn_tensor_desc pre_tfrmd_desc, tfrmd_desc;
  // pre-transformed desc.
  int rank = fShape.size();
  zdnn_data_layouts zDNNLayout = (rank == 2) ? ZDNN_2DS : ZDNN_3DS;
  zdnn_data_types zDNNType = mlirTypeToZDNNType(elementType);
  set_info_pre_transformed_desc(&pre_tfrmd_desc, zDNNLayout, zDNNType, fShape);
  // transformed desc.
  zdnn_concat_info concatInfo = RNN_TYPE_LSTM |
                                ((rank == 2) ? USAGE_BIASES : USAGE_WEIGHTS) |
                                PREV_LAYER_NONE;
  zdnn_status status = generate_transformed_desc_concatenated(
      &pre_tfrmd_desc, concatInfo, &tfrmd_desc);
  assert(status == ZDNN_OK);
  // Stick data using the software stickify.
  zdnn_ztensor ztensor;
  init_ztensor(&pre_tfrmd_desc, &tfrmd_desc, &ztensor);
  status = allochelper_ztensor_alloc(&ztensor);
  assert(status == ZDNN_OK);
  status = stickify(&ztensor, rawFData.data(), rawIData.data(), rawCData.data(),
      rawOData.data());
  assert(status == ZDNN_OK);

  // Emit a constant global in ZHigh dialect.
  ZHighStickifiedConstantOp constantOp = emitZHighStickifiedConstant(
      rewriter, loc, &ztensor, replacingValue.getType());

  return constantOp;
}

ZHighStickifiedConstantOp createConstantForStickForGRU(
    PatternRewriter &rewriter, Value replacingValue, Value inputZ, Value inputR,
    Value inputH) {
  Location loc = replacingValue.getLoc();
  Operation *zOp = inputZ.getDefiningOp();
  Operation *rOp = inputR.getDefiningOp();
  Operation *hOp = inputH.getDefiningOp();

  ArrayRef<int64_t> zShape =
      mlir::cast<ShapedType>(inputZ.getType()).getShape();
  assert((zShape.size() == 2 || zShape.size() == 3) && "Wrong tensor shape");
  Type elementType = mlir::cast<ShapedType>(inputZ.getType()).getElementType();

  // Read dense attributes.
  DenseElementsAttr zDataAttr = mlir::dyn_cast_or_null<mlir::DenseElementsAttr>(
      zOp->getAttrOfType<::mlir::Attribute>("value"));
  DenseElementsAttr rDataAttr = mlir::dyn_cast_or_null<mlir::DenseElementsAttr>(
      rOp->getAttrOfType<::mlir::Attribute>("value"));
  DenseElementsAttr hDataAttr = mlir::dyn_cast_or_null<mlir::DenseElementsAttr>(
      hOp->getAttrOfType<::mlir::Attribute>("value"));
  assert((zDataAttr && rDataAttr && hDataAttr) && "Attribute is null");
  // Read attributes's raw data.
  std::vector<char> rawZData, rawHData, rawRData, rawOData;
  getRawData(zDataAttr, rawZData);
  getRawData(rDataAttr, rawRData);
  getRawData(hDataAttr, rawHData);

  // Call stickify.
  zdnn_tensor_desc pre_tfrmd_desc, tfrmd_desc;
  // pre-transformed desc.
  int rank = zShape.size();
  zdnn_data_layouts zDNNLayout = (rank == 2) ? ZDNN_2DS : ZDNN_3DS;
  zdnn_data_types zDNNType = mlirTypeToZDNNType(elementType);
  set_info_pre_transformed_desc(&pre_tfrmd_desc, zDNNLayout, zDNNType, zShape);
  // transformed desc.
  zdnn_concat_info concatInfo = RNN_TYPE_GRU |
                                ((rank == 2) ? USAGE_BIASES : USAGE_WEIGHTS) |
                                PREV_LAYER_NONE;
  zdnn_status status = generate_transformed_desc_concatenated(
      &pre_tfrmd_desc, concatInfo, &tfrmd_desc);
  assert(status == ZDNN_OK);
  // Stick data using the software stickify.
  zdnn_ztensor ztensor;
  init_ztensor(&pre_tfrmd_desc, &tfrmd_desc, &ztensor);
  status = allochelper_ztensor_alloc(&ztensor);
  assert(status == ZDNN_OK);
  status =
      stickify(&ztensor, rawZData.data(), rawRData.data(), rawHData.data());
  assert(status == ZDNN_OK);
  // Emit a constant global in ZHigh dialect.
  ZHighStickifiedConstantOp constantOp = emitZHighStickifiedConstant(
      rewriter, loc, &ztensor, replacingValue.getType());

  return constantOp;
}

//===----------------------------------------------------------------------===//
// ZHigh Stick to Krnl Lowering Pass
//===----------------------------------------------------------------------===//

namespace {
/// Use anonymous namespace to avoid duplication symbol `populateWithGenerated`
/// among multiple tablegen-based definitions.

/// Include the patterns defined in the Declarative Rewrite framework.
#include "src/Accelerators/NNPA/Transform/ZHigh/ONNXZHighConstPropagation.inc"

struct ZHighConstPropagationPass
    //: public PassWrapper<ZHighConstPropagationPass, OperationPass<ModuleOp>> {
    : public PassWrapper<ZHighConstPropagationPass,
          OperationPass<func::FuncOp>> {

  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ZHighConstPropagationPass)

  StringRef getArgument() const override { return "constprop-zhigh"; }

  StringRef getDescription() const override {
    return "Constant propagation for ZHigh operations.";
  }

  void runOnOperation() override {
    auto function = getOperation();
    ConversionTarget target(getContext());
    RewritePatternSet patterns(&getContext());
    populateWithGenerated(patterns);
    (void)applyPatternsAndFoldGreedily(function, std::move(patterns));
  }
};
} // anonymous namespace

std::unique_ptr<Pass> createZHighConstPropagationPass() {
  return std::make_unique<ZHighConstPropagationPass>();
}

} // namespace zhigh
} // namespace onnx_mlir
