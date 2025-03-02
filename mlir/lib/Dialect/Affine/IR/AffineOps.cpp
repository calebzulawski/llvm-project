//===- AffineOps.cpp - MLIR Affine Operations -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Affine/IR/AffineValueMap.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/AffineExprVisitor.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Interfaces/ShapedOpInterfaces.h"
#include "mlir/Support/MathExtras.h"
#include "mlir/Transforms/InliningUtils.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallVectorExtras.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Debug.h"
#include <numeric>
#include <optional>

using namespace mlir;
using namespace mlir::affine;

#define DEBUG_TYPE "affine-ops"

#include "mlir/Dialect/Affine/IR/AffineOpsDialect.cpp.inc"

/// A utility function to check if a value is defined at the top level of
/// `region` or is an argument of `region`. A value of index type defined at the
/// top level of a `AffineScope` region is always a valid symbol for all
/// uses in that region.
bool mlir::affine::isTopLevelValue(Value value, Region *region) {
  if (auto arg = llvm::dyn_cast<BlockArgument>(value))
    return arg.getParentRegion() == region;
  return value.getDefiningOp()->getParentRegion() == region;
}

/// Checks if `value` known to be a legal affine dimension or symbol in `src`
/// region remains legal if the operation that uses it is inlined into `dest`
/// with the given value mapping. `legalityCheck` is either `isValidDim` or
/// `isValidSymbol`, depending on the value being required to remain a valid
/// dimension or symbol.
static bool
remainsLegalAfterInline(Value value, Region *src, Region *dest,
                        const IRMapping &mapping,
                        function_ref<bool(Value, Region *)> legalityCheck) {
  // If the value is a valid dimension for any other reason than being
  // a top-level value, it will remain valid: constants get inlined
  // with the function, transitive affine applies also get inlined and
  // will be checked themselves, etc.
  if (!isTopLevelValue(value, src))
    return true;

  // If it's a top-level value because it's a block operand, i.e. a
  // function argument, check whether the value replacing it after
  // inlining is a valid dimension in the new region.
  if (llvm::isa<BlockArgument>(value))
    return legalityCheck(mapping.lookup(value), dest);

  // If it's a top-level value because it's defined in the region,
  // it can only be inlined if the defining op is a constant or a
  // `dim`, which can appear anywhere and be valid, since the defining
  // op won't be top-level anymore after inlining.
  Attribute operandCst;
  bool isDimLikeOp = isa<ShapedDimOpInterface>(value.getDefiningOp());
  return matchPattern(value.getDefiningOp(), m_Constant(&operandCst)) ||
         isDimLikeOp;
}

/// Checks if all values known to be legal affine dimensions or symbols in `src`
/// remain so if their respective users are inlined into `dest`.
static bool
remainsLegalAfterInline(ValueRange values, Region *src, Region *dest,
                        const IRMapping &mapping,
                        function_ref<bool(Value, Region *)> legalityCheck) {
  return llvm::all_of(values, [&](Value v) {
    return remainsLegalAfterInline(v, src, dest, mapping, legalityCheck);
  });
}

/// Checks if an affine read or write operation remains legal after inlining
/// from `src` to `dest`.
template <typename OpTy>
static bool remainsLegalAfterInline(OpTy op, Region *src, Region *dest,
                                    const IRMapping &mapping) {
  static_assert(llvm::is_one_of<OpTy, AffineReadOpInterface,
                                AffineWriteOpInterface>::value,
                "only ops with affine read/write interface are supported");

  AffineMap map = op.getAffineMap();
  ValueRange dimOperands = op.getMapOperands().take_front(map.getNumDims());
  ValueRange symbolOperands =
      op.getMapOperands().take_back(map.getNumSymbols());
  if (!remainsLegalAfterInline(
          dimOperands, src, dest, mapping,
          static_cast<bool (*)(Value, Region *)>(isValidDim)))
    return false;
  if (!remainsLegalAfterInline(
          symbolOperands, src, dest, mapping,
          static_cast<bool (*)(Value, Region *)>(isValidSymbol)))
    return false;
  return true;
}

/// Checks if an affine apply operation remains legal after inlining from `src`
/// to `dest`.
//  Use "unused attribute" marker to silence clang-tidy warning stemming from
//  the inability to see through "llvm::TypeSwitch".
template <>
bool LLVM_ATTRIBUTE_UNUSED remainsLegalAfterInline(AffineApplyOp op,
                                                   Region *src, Region *dest,
                                                   const IRMapping &mapping) {
  // If it's a valid dimension, we need to check that it remains so.
  if (isValidDim(op.getResult(), src))
    return remainsLegalAfterInline(
        op.getMapOperands(), src, dest, mapping,
        static_cast<bool (*)(Value, Region *)>(isValidDim));

  // Otherwise it must be a valid symbol, check that it remains so.
  return remainsLegalAfterInline(
      op.getMapOperands(), src, dest, mapping,
      static_cast<bool (*)(Value, Region *)>(isValidSymbol));
}

//===----------------------------------------------------------------------===//
// AffineDialect Interfaces
//===----------------------------------------------------------------------===//

namespace {
/// This class defines the interface for handling inlining with affine
/// operations.
struct AffineInlinerInterface : public DialectInlinerInterface {
  using DialectInlinerInterface::DialectInlinerInterface;

  //===--------------------------------------------------------------------===//
  // Analysis Hooks
  //===--------------------------------------------------------------------===//

  /// Returns true if the given region 'src' can be inlined into the region
  /// 'dest' that is attached to an operation registered to the current dialect.
  /// 'wouldBeCloned' is set if the region is cloned into its new location
  /// rather than moved, indicating there may be other users.
  bool isLegalToInline(Region *dest, Region *src, bool wouldBeCloned,
                       IRMapping &valueMapping) const final {
    // We can inline into affine loops and conditionals if this doesn't break
    // affine value categorization rules.
    Operation *destOp = dest->getParentOp();
    if (!isa<AffineParallelOp, AffineForOp, AffineIfOp>(destOp))
      return false;

    // Multi-block regions cannot be inlined into affine constructs, all of
    // which require single-block regions.
    if (!llvm::hasSingleElement(*src))
      return false;

    // Side-effecting operations that the affine dialect cannot understand
    // should not be inlined.
    Block &srcBlock = src->front();
    for (Operation &op : srcBlock) {
      // Ops with no side effects are fine,
      if (auto iface = dyn_cast<MemoryEffectOpInterface>(op)) {
        if (iface.hasNoEffect())
          continue;
      }

      // Assuming the inlined region is valid, we only need to check if the
      // inlining would change it.
      bool remainsValid =
          llvm::TypeSwitch<Operation *, bool>(&op)
              .Case<AffineApplyOp, AffineReadOpInterface,
                    AffineWriteOpInterface>([&](auto op) {
                return remainsLegalAfterInline(op, src, dest, valueMapping);
              })
              .Default([](Operation *) {
                // Conservatively disallow inlining ops we cannot reason about.
                return false;
              });

      if (!remainsValid)
        return false;
    }

    return true;
  }

  /// Returns true if the given operation 'op', that is registered to this
  /// dialect, can be inlined into the given region, false otherwise.
  bool isLegalToInline(Operation *op, Region *region, bool wouldBeCloned,
                       IRMapping &valueMapping) const final {
    // Always allow inlining affine operations into a region that is marked as
    // affine scope, or into affine loops and conditionals. There are some edge
    // cases when inlining *into* affine structures, but that is handled in the
    // other 'isLegalToInline' hook above.
    Operation *parentOp = region->getParentOp();
    return parentOp->hasTrait<OpTrait::AffineScope>() ||
           isa<AffineForOp, AffineParallelOp, AffineIfOp>(parentOp);
  }

  /// Affine regions should be analyzed recursively.
  bool shouldAnalyzeRecursively(Operation *op) const final { return true; }
};
} // namespace

//===----------------------------------------------------------------------===//
// AffineDialect
//===----------------------------------------------------------------------===//

void AffineDialect::initialize() {
  addOperations<AffineDmaStartOp, AffineDmaWaitOp,
#define GET_OP_LIST
#include "mlir/Dialect/Affine/IR/AffineOps.cpp.inc"
                >();
  addInterfaces<AffineInlinerInterface>();
}

/// Materialize a single constant operation from a given attribute value with
/// the desired resultant type.
Operation *AffineDialect::materializeConstant(OpBuilder &builder,
                                              Attribute value, Type type,
                                              Location loc) {
  return arith::ConstantOp::materialize(builder, value, type, loc);
}

/// A utility function to check if a value is defined at the top level of an
/// op with trait `AffineScope`. If the value is defined in an unlinked region,
/// conservatively assume it is not top-level. A value of index type defined at
/// the top level is always a valid symbol.
bool mlir::affine::isTopLevelValue(Value value) {
  if (auto arg = llvm::dyn_cast<BlockArgument>(value)) {
    // The block owning the argument may be unlinked, e.g. when the surrounding
    // region has not yet been attached to an Op, at which point the parent Op
    // is null.
    Operation *parentOp = arg.getOwner()->getParentOp();
    return parentOp && parentOp->hasTrait<OpTrait::AffineScope>();
  }
  // The defining Op may live in an unlinked block so its parent Op may be null.
  Operation *parentOp = value.getDefiningOp()->getParentOp();
  return parentOp && parentOp->hasTrait<OpTrait::AffineScope>();
}

/// Returns the closest region enclosing `op` that is held by an operation with
/// trait `AffineScope`; `nullptr` if there is no such region.
Region *mlir::affine::getAffineScope(Operation *op) {
  auto *curOp = op;
  while (auto *parentOp = curOp->getParentOp()) {
    if (parentOp->hasTrait<OpTrait::AffineScope>())
      return curOp->getParentRegion();
    curOp = parentOp;
  }
  return nullptr;
}

// A Value can be used as a dimension id iff it meets one of the following
// conditions:
// *) It is valid as a symbol.
// *) It is an induction variable.
// *) It is the result of affine apply operation with dimension id arguments.
bool mlir::affine::isValidDim(Value value) {
  // The value must be an index type.
  if (!value.getType().isIndex())
    return false;

  if (auto *defOp = value.getDefiningOp())
    return isValidDim(value, getAffineScope(defOp));

  // This value has to be a block argument for an op that has the
  // `AffineScope` trait or for an affine.for or affine.parallel.
  auto *parentOp = llvm::cast<BlockArgument>(value).getOwner()->getParentOp();
  return parentOp && (parentOp->hasTrait<OpTrait::AffineScope>() ||
                      isa<AffineForOp, AffineParallelOp>(parentOp));
}

// Value can be used as a dimension id iff it meets one of the following
// conditions:
// *) It is valid as a symbol.
// *) It is an induction variable.
// *) It is the result of an affine apply operation with dimension id operands.
bool mlir::affine::isValidDim(Value value, Region *region) {
  // The value must be an index type.
  if (!value.getType().isIndex())
    return false;

  // All valid symbols are okay.
  if (isValidSymbol(value, region))
    return true;

  auto *op = value.getDefiningOp();
  if (!op) {
    // This value has to be a block argument for an affine.for or an
    // affine.parallel.
    auto *parentOp = llvm::cast<BlockArgument>(value).getOwner()->getParentOp();
    return isa<AffineForOp, AffineParallelOp>(parentOp);
  }

  // Affine apply operation is ok if all of its operands are ok.
  if (auto applyOp = dyn_cast<AffineApplyOp>(op))
    return applyOp.isValidDim(region);
  // The dim op is okay if its operand memref/tensor is defined at the top
  // level.
  if (auto dimOp = dyn_cast<ShapedDimOpInterface>(op))
    return isTopLevelValue(dimOp.getShapedValue());
  return false;
}

/// Returns true if the 'index' dimension of the `memref` defined by
/// `memrefDefOp` is a statically  shaped one or defined using a valid symbol
/// for `region`.
template <typename AnyMemRefDefOp>
static bool isMemRefSizeValidSymbol(AnyMemRefDefOp memrefDefOp, unsigned index,
                                    Region *region) {
  auto memRefType = memrefDefOp.getType();
  // Statically shaped.
  if (!memRefType.isDynamicDim(index))
    return true;
  // Get the position of the dimension among dynamic dimensions;
  unsigned dynamicDimPos = memRefType.getDynamicDimIndex(index);
  return isValidSymbol(*(memrefDefOp.getDynamicSizes().begin() + dynamicDimPos),
                       region);
}

/// Returns true if the result of the dim op is a valid symbol for `region`.
static bool isDimOpValidSymbol(ShapedDimOpInterface dimOp, Region *region) {
  // The dim op is okay if its source is defined at the top level.
  if (isTopLevelValue(dimOp.getShapedValue()))
    return true;

  // Conservatively handle remaining BlockArguments as non-valid symbols.
  // E.g. scf.for iterArgs.
  if (llvm::isa<BlockArgument>(dimOp.getShapedValue()))
    return false;

  // The dim op is also okay if its operand memref is a view/subview whose
  // corresponding size is a valid symbol.
  std::optional<int64_t> index = getConstantIntValue(dimOp.getDimension());

  // Be conservative if we can't understand the dimension.
  if (!index.has_value())
    return false;

  int64_t i = index.value();
  return TypeSwitch<Operation *, bool>(dimOp.getShapedValue().getDefiningOp())
      .Case<memref::ViewOp, memref::SubViewOp, memref::AllocOp>(
          [&](auto op) { return isMemRefSizeValidSymbol(op, i, region); })
      .Default([](Operation *) { return false; });
}

// A value can be used as a symbol (at all its use sites) iff it meets one of
// the following conditions:
// *) It is a constant.
// *) Its defining op or block arg appearance is immediately enclosed by an op
//    with `AffineScope` trait.
// *) It is the result of an affine.apply operation with symbol operands.
// *) It is a result of the dim op on a memref whose corresponding size is a
//    valid symbol.
bool mlir::affine::isValidSymbol(Value value) {
  if (!value)
    return false;

  // The value must be an index type.
  if (!value.getType().isIndex())
    return false;

  // Check that the value is a top level value.
  if (isTopLevelValue(value))
    return true;

  if (auto *defOp = value.getDefiningOp())
    return isValidSymbol(value, getAffineScope(defOp));

  return false;
}

/// A value can be used as a symbol for `region` iff it meets one of the
/// following conditions:
/// *) It is a constant.
/// *) It is the result of an affine apply operation with symbol arguments.
/// *) It is a result of the dim op on a memref whose corresponding size is
///    a valid symbol.
/// *) It is defined at the top level of 'region' or is its argument.
/// *) It dominates `region`'s parent op.
/// If `region` is null, conservatively assume the symbol definition scope does
/// not exist and only accept the values that would be symbols regardless of
/// the surrounding region structure, i.e. the first three cases above.
bool mlir::affine::isValidSymbol(Value value, Region *region) {
  // The value must be an index type.
  if (!value.getType().isIndex())
    return false;

  // A top-level value is a valid symbol.
  if (region && ::isTopLevelValue(value, region))
    return true;

  auto *defOp = value.getDefiningOp();
  if (!defOp) {
    // A block argument that is not a top-level value is a valid symbol if it
    // dominates region's parent op.
    Operation *regionOp = region ? region->getParentOp() : nullptr;
    if (regionOp && !regionOp->hasTrait<OpTrait::IsIsolatedFromAbove>())
      if (auto *parentOpRegion = region->getParentOp()->getParentRegion())
        return isValidSymbol(value, parentOpRegion);
    return false;
  }

  // Constant operation is ok.
  Attribute operandCst;
  if (matchPattern(defOp, m_Constant(&operandCst)))
    return true;

  // Affine apply operation is ok if all of its operands are ok.
  if (auto applyOp = dyn_cast<AffineApplyOp>(defOp))
    return applyOp.isValidSymbol(region);

  // Dim op results could be valid symbols at any level.
  if (auto dimOp = dyn_cast<ShapedDimOpInterface>(defOp))
    return isDimOpValidSymbol(dimOp, region);

  // Check for values dominating `region`'s parent op.
  Operation *regionOp = region ? region->getParentOp() : nullptr;
  if (regionOp && !regionOp->hasTrait<OpTrait::IsIsolatedFromAbove>())
    if (auto *parentRegion = region->getParentOp()->getParentRegion())
      return isValidSymbol(value, parentRegion);

  return false;
}

// Returns true if 'value' is a valid index to an affine operation (e.g.
// affine.load, affine.store, affine.dma_start, affine.dma_wait) where
// `region` provides the polyhedral symbol scope. Returns false otherwise.
static bool isValidAffineIndexOperand(Value value, Region *region) {
  return isValidDim(value, region) || isValidSymbol(value, region);
}

/// Prints dimension and symbol list.
static void printDimAndSymbolList(Operation::operand_iterator begin,
                                  Operation::operand_iterator end,
                                  unsigned numDims, OpAsmPrinter &printer) {
  OperandRange operands(begin, end);
  printer << '(' << operands.take_front(numDims) << ')';
  if (operands.size() > numDims)
    printer << '[' << operands.drop_front(numDims) << ']';
}

/// Parses dimension and symbol list and returns true if parsing failed.
ParseResult mlir::affine::parseDimAndSymbolList(
    OpAsmParser &parser, SmallVectorImpl<Value> &operands, unsigned &numDims) {
  SmallVector<OpAsmParser::UnresolvedOperand, 8> opInfos;
  if (parser.parseOperandList(opInfos, OpAsmParser::Delimiter::Paren))
    return failure();
  // Store number of dimensions for validation by caller.
  numDims = opInfos.size();

  // Parse the optional symbol operands.
  auto indexTy = parser.getBuilder().getIndexType();
  return failure(parser.parseOperandList(
                     opInfos, OpAsmParser::Delimiter::OptionalSquare) ||
                 parser.resolveOperands(opInfos, indexTy, operands));
}

/// Utility function to verify that a set of operands are valid dimension and
/// symbol identifiers. The operands should be laid out such that the dimension
/// operands are before the symbol operands. This function returns failure if
/// there was an invalid operand. An operation is provided to emit any necessary
/// errors.
template <typename OpTy>
static LogicalResult
verifyDimAndSymbolIdentifiers(OpTy &op, Operation::operand_range operands,
                              unsigned numDims) {
  unsigned opIt = 0;
  for (auto operand : operands) {
    if (opIt++ < numDims) {
      if (!isValidDim(operand, getAffineScope(op)))
        return op.emitOpError("operand cannot be used as a dimension id");
    } else if (!isValidSymbol(operand, getAffineScope(op))) {
      return op.emitOpError("operand cannot be used as a symbol");
    }
  }
  return success();
}

//===----------------------------------------------------------------------===//
// AffineApplyOp
//===----------------------------------------------------------------------===//

AffineValueMap AffineApplyOp::getAffineValueMap() {
  return AffineValueMap(getAffineMap(), getOperands(), getResult());
}

ParseResult AffineApplyOp::parse(OpAsmParser &parser, OperationState &result) {
  auto &builder = parser.getBuilder();
  auto indexTy = builder.getIndexType();

  AffineMapAttr mapAttr;
  unsigned numDims;
  if (parser.parseAttribute(mapAttr, "map", result.attributes) ||
      parseDimAndSymbolList(parser, result.operands, numDims) ||
      parser.parseOptionalAttrDict(result.attributes))
    return failure();
  auto map = mapAttr.getValue();

  if (map.getNumDims() != numDims ||
      numDims + map.getNumSymbols() != result.operands.size()) {
    return parser.emitError(parser.getNameLoc(),
                            "dimension or symbol index mismatch");
  }

  result.types.append(map.getNumResults(), indexTy);
  return success();
}

void AffineApplyOp::print(OpAsmPrinter &p) {
  p << " " << getMapAttr();
  printDimAndSymbolList(operand_begin(), operand_end(),
                        getAffineMap().getNumDims(), p);
  p.printOptionalAttrDict((*this)->getAttrs(), /*elidedAttrs=*/{"map"});
}

LogicalResult AffineApplyOp::verify() {
  // Check input and output dimensions match.
  AffineMap affineMap = getMap();

  // Verify that operand count matches affine map dimension and symbol count.
  if (getNumOperands() != affineMap.getNumDims() + affineMap.getNumSymbols())
    return emitOpError(
        "operand count and affine map dimension and symbol count must match");

  // Verify that the map only produces one result.
  if (affineMap.getNumResults() != 1)
    return emitOpError("mapping must produce one value");

  return success();
}

// The result of the affine apply operation can be used as a dimension id if all
// its operands are valid dimension ids.
bool AffineApplyOp::isValidDim() {
  return llvm::all_of(getOperands(),
                      [](Value op) { return affine::isValidDim(op); });
}

// The result of the affine apply operation can be used as a dimension id if all
// its operands are valid dimension ids with the parent operation of `region`
// defining the polyhedral scope for symbols.
bool AffineApplyOp::isValidDim(Region *region) {
  return llvm::all_of(getOperands(),
                      [&](Value op) { return ::isValidDim(op, region); });
}

// The result of the affine apply operation can be used as a symbol if all its
// operands are symbols.
bool AffineApplyOp::isValidSymbol() {
  return llvm::all_of(getOperands(),
                      [](Value op) { return affine::isValidSymbol(op); });
}

// The result of the affine apply operation can be used as a symbol in `region`
// if all its operands are symbols in `region`.
bool AffineApplyOp::isValidSymbol(Region *region) {
  return llvm::all_of(getOperands(), [&](Value operand) {
    return affine::isValidSymbol(operand, region);
  });
}

OpFoldResult AffineApplyOp::fold(FoldAdaptor adaptor) {
  auto map = getAffineMap();

  // Fold dims and symbols to existing values.
  auto expr = map.getResult(0);
  if (auto dim = expr.dyn_cast<AffineDimExpr>())
    return getOperand(dim.getPosition());
  if (auto sym = expr.dyn_cast<AffineSymbolExpr>())
    return getOperand(map.getNumDims() + sym.getPosition());

  // Otherwise, default to folding the map.
  SmallVector<Attribute, 1> result;
  if (failed(map.constantFold(adaptor.getMapOperands(), result)))
    return {};
  return result[0];
}

/// Returns the largest known divisor of `e`. Exploits information from the
/// values in `operands`.
static int64_t getLargestKnownDivisor(AffineExpr e, ArrayRef<Value> operands) {
  // This method isn't aware of `operands`.
  int64_t div = e.getLargestKnownDivisor();

  // We now make use of operands for the case `e` is a dim expression.
  // TODO: More powerful simplification would have to modify
  // getLargestKnownDivisor to take `operands` and exploit that information as
  // well for dim/sym expressions, but in that case, getLargestKnownDivisor
  // can't be part of the IR library but of the `Analysis` library. The IR
  // library can only really depend on simple O(1) checks.
  auto dimExpr = e.dyn_cast<AffineDimExpr>();
  // If it's not a dim expr, `div` is the best we have.
  if (!dimExpr)
    return div;

  // We simply exploit information from loop IVs.
  // We don't need to use mlir::getLargestKnownDivisorOfValue since the other
  // desired simplifications are expected to be part of other
  // canonicalizations. Also, mlir::getLargestKnownDivisorOfValue is part of the
  // LoopAnalysis library.
  Value operand = operands[dimExpr.getPosition()];
  int64_t operandDivisor = 1;
  // TODO: With the right accessors, this can be extended to
  // LoopLikeOpInterface.
  if (AffineForOp forOp = getForInductionVarOwner(operand)) {
    if (forOp.hasConstantLowerBound() && forOp.getConstantLowerBound() == 0) {
      operandDivisor = forOp.getStep();
    } else {
      uint64_t lbLargestKnownDivisor =
          forOp.getLowerBoundMap().getLargestKnownDivisorOfMapExprs();
      operandDivisor = std::gcd(lbLargestKnownDivisor, forOp.getStep());
    }
  }
  return operandDivisor;
}

/// Check if `e` is known to be: 0 <= `e` < `k`. Handles the simple cases of `e`
/// being an affine dim expression or a constant.
static bool isNonNegativeBoundedBy(AffineExpr e, ArrayRef<Value> operands,
                                   int64_t k) {
  if (auto constExpr = e.dyn_cast<AffineConstantExpr>()) {
    int64_t constVal = constExpr.getValue();
    return constVal >= 0 && constVal < k;
  }
  auto dimExpr = e.dyn_cast<AffineDimExpr>();
  if (!dimExpr)
    return false;
  Value operand = operands[dimExpr.getPosition()];
  // TODO: With the right accessors, this can be extended to
  // LoopLikeOpInterface.
  if (AffineForOp forOp = getForInductionVarOwner(operand)) {
    if (forOp.hasConstantLowerBound() && forOp.getConstantLowerBound() >= 0 &&
        forOp.hasConstantUpperBound() && forOp.getConstantUpperBound() <= k) {
      return true;
    }
  }

  // We don't consider other cases like `operand` being defined by a constant or
  // an affine.apply op since such cases will already be handled by other
  // patterns and propagation of loop IVs or constant would happen.
  return false;
}

/// Check if expression `e` is of the form d*e_1 + e_2 where 0 <= e_2 < d.
/// Set `div` to `d`, `quotientTimesDiv` to e_1 and `rem` to e_2 if the
/// expression is in that form.
static bool isQTimesDPlusR(AffineExpr e, ArrayRef<Value> operands, int64_t &div,
                           AffineExpr &quotientTimesDiv, AffineExpr &rem) {
  auto bin = e.dyn_cast<AffineBinaryOpExpr>();
  if (!bin || bin.getKind() != AffineExprKind::Add)
    return false;

  AffineExpr llhs = bin.getLHS();
  AffineExpr rlhs = bin.getRHS();
  div = getLargestKnownDivisor(llhs, operands);
  if (isNonNegativeBoundedBy(rlhs, operands, div)) {
    quotientTimesDiv = llhs;
    rem = rlhs;
    return true;
  }
  div = getLargestKnownDivisor(rlhs, operands);
  if (isNonNegativeBoundedBy(llhs, operands, div)) {
    quotientTimesDiv = rlhs;
    rem = llhs;
    return true;
  }
  return false;
}

/// Gets the constant lower bound on an `iv`.
static std::optional<int64_t> getLowerBound(Value iv) {
  AffineForOp forOp = getForInductionVarOwner(iv);
  if (forOp && forOp.hasConstantLowerBound())
    return forOp.getConstantLowerBound();
  return std::nullopt;
}

/// Gets the constant upper bound on an affine.for `iv`.
static std::optional<int64_t> getUpperBound(Value iv) {
  AffineForOp forOp = getForInductionVarOwner(iv);
  if (!forOp || !forOp.hasConstantUpperBound())
    return std::nullopt;

  // If its lower bound is also known, we can get a more precise bound
  // whenever the step is not one.
  if (forOp.hasConstantLowerBound()) {
    return forOp.getConstantUpperBound() - 1 -
           (forOp.getConstantUpperBound() - forOp.getConstantLowerBound() - 1) %
               forOp.getStep();
  }
  return forOp.getConstantUpperBound() - 1;
}

/// Get a lower or upper (depending on `isUpper`) bound for `expr` while using
/// the constant lower and upper bounds for its inputs provided in
/// `constLowerBounds` and `constUpperBounds`. Return std::nullopt if such a
/// bound can't be computed. This method only handles simple sum of product
/// expressions (w.r.t constant coefficients) so as to not depend on anything
/// heavyweight in `Analysis`. Expressions of the form: c0*d0 + c1*d1 + c2*s0 +
/// ... + c_n are handled. Expressions involving floordiv, ceildiv, mod or
/// semi-affine ones will lead std::nullopt being returned.
static std::optional<int64_t>
getBoundForExpr(AffineExpr expr, unsigned numDims, unsigned numSymbols,
                ArrayRef<std::optional<int64_t>> constLowerBounds,
                ArrayRef<std::optional<int64_t>> constUpperBounds,
                bool isUpper) {
  // Handle divs and mods.
  if (auto binOpExpr = expr.dyn_cast<AffineBinaryOpExpr>()) {
    // If the LHS of a floor or ceil is bounded and the RHS is a constant, we
    // can compute an upper bound.
    if (binOpExpr.getKind() == AffineExprKind::FloorDiv) {
      auto rhsConst = binOpExpr.getRHS().dyn_cast<AffineConstantExpr>();
      if (!rhsConst || rhsConst.getValue() < 1)
        return std::nullopt;
      auto bound = getBoundForExpr(binOpExpr.getLHS(), numDims, numSymbols,
                                   constLowerBounds, constUpperBounds, isUpper);
      if (!bound)
        return std::nullopt;
      return mlir::floorDiv(*bound, rhsConst.getValue());
    }
    if (binOpExpr.getKind() == AffineExprKind::CeilDiv) {
      auto rhsConst = binOpExpr.getRHS().dyn_cast<AffineConstantExpr>();
      if (rhsConst && rhsConst.getValue() >= 1) {
        auto bound =
            getBoundForExpr(binOpExpr.getLHS(), numDims, numSymbols,
                            constLowerBounds, constUpperBounds, isUpper);
        if (!bound)
          return std::nullopt;
        return mlir::ceilDiv(*bound, rhsConst.getValue());
      }
      return std::nullopt;
    }
    if (binOpExpr.getKind() == AffineExprKind::Mod) {
      // lhs mod c is always <= c - 1 and non-negative. In addition, if `lhs` is
      // bounded such that lb <= lhs <= ub and lb floordiv c == ub floordiv c
      // (same "interval"), then lb mod c <= lhs mod c <= ub mod c.
      auto rhsConst = binOpExpr.getRHS().dyn_cast<AffineConstantExpr>();
      if (rhsConst && rhsConst.getValue() >= 1) {
        int64_t rhsConstVal = rhsConst.getValue();
        auto lb = getBoundForExpr(binOpExpr.getLHS(), numDims, numSymbols,
                                  constLowerBounds, constUpperBounds,
                                  /*isUpper=*/false);
        auto ub = getBoundForExpr(binOpExpr.getLHS(), numDims, numSymbols,
                                  constLowerBounds, constUpperBounds, isUpper);
        if (ub && lb &&
            floorDiv(*lb, rhsConstVal) == floorDiv(*ub, rhsConstVal))
          return isUpper ? mod(*ub, rhsConstVal) : mod(*lb, rhsConstVal);
        return isUpper ? rhsConstVal - 1 : 0;
      }
    }
  }
  // Flatten the expression.
  SimpleAffineExprFlattener flattener(numDims, numSymbols);
  flattener.walkPostOrder(expr);
  ArrayRef<int64_t> flattenedExpr = flattener.operandExprStack.back();
  // TODO: Handle local variables. We can get hold of flattener.localExprs and
  // get bound on the local expr recursively.
  if (flattener.numLocals > 0)
    return std::nullopt;
  int64_t bound = 0;
  // Substitute the constant lower or upper bound for the dimensional or
  // symbolic input depending on `isUpper` to determine the bound.
  for (unsigned i = 0, e = numDims + numSymbols; i < e; ++i) {
    if (flattenedExpr[i] > 0) {
      auto &constBound = isUpper ? constUpperBounds[i] : constLowerBounds[i];
      if (!constBound)
        return std::nullopt;
      bound += *constBound * flattenedExpr[i];
    } else if (flattenedExpr[i] < 0) {
      auto &constBound = isUpper ? constLowerBounds[i] : constUpperBounds[i];
      if (!constBound)
        return std::nullopt;
      bound += *constBound * flattenedExpr[i];
    }
  }
  // Constant term.
  bound += flattenedExpr.back();
  return bound;
}

/// Determine a constant upper bound for `expr` if one exists while exploiting
/// values in `operands`. Note that the upper bound is an inclusive one. `expr`
/// is guaranteed to be less than or equal to it.
static std::optional<int64_t> getUpperBound(AffineExpr expr, unsigned numDims,
                                            unsigned numSymbols,
                                            ArrayRef<Value> operands) {
  // Get the constant lower or upper bounds on the operands.
  SmallVector<std::optional<int64_t>> constLowerBounds, constUpperBounds;
  constLowerBounds.reserve(operands.size());
  constUpperBounds.reserve(operands.size());
  for (Value operand : operands) {
    constLowerBounds.push_back(getLowerBound(operand));
    constUpperBounds.push_back(getUpperBound(operand));
  }

  if (auto constExpr = expr.dyn_cast<AffineConstantExpr>())
    return constExpr.getValue();

  return getBoundForExpr(expr, numDims, numSymbols, constLowerBounds,
                         constUpperBounds,
                         /*isUpper=*/true);
}

/// Determine a constant lower bound for `expr` if one exists while exploiting
/// values in `operands`. Note that the upper bound is an inclusive one. `expr`
/// is guaranteed to be less than or equal to it.
static std::optional<int64_t> getLowerBound(AffineExpr expr, unsigned numDims,
                                            unsigned numSymbols,
                                            ArrayRef<Value> operands) {
  // Get the constant lower or upper bounds on the operands.
  SmallVector<std::optional<int64_t>> constLowerBounds, constUpperBounds;
  constLowerBounds.reserve(operands.size());
  constUpperBounds.reserve(operands.size());
  for (Value operand : operands) {
    constLowerBounds.push_back(getLowerBound(operand));
    constUpperBounds.push_back(getUpperBound(operand));
  }

  std::optional<int64_t> lowerBound;
  if (auto constExpr = expr.dyn_cast<AffineConstantExpr>()) {
    lowerBound = constExpr.getValue();
  } else {
    lowerBound = getBoundForExpr(expr, numDims, numSymbols, constLowerBounds,
                                 constUpperBounds,
                                 /*isUpper=*/false);
  }
  return lowerBound;
}

/// Simplify `expr` while exploiting information from the values in `operands`.
static void simplifyExprAndOperands(AffineExpr &expr, unsigned numDims,
                                    unsigned numSymbols,
                                    ArrayRef<Value> operands) {
  // We do this only for certain floordiv/mod expressions.
  auto binExpr = expr.dyn_cast<AffineBinaryOpExpr>();
  if (!binExpr)
    return;

  // Simplify the child expressions first.
  AffineExpr lhs = binExpr.getLHS();
  AffineExpr rhs = binExpr.getRHS();
  simplifyExprAndOperands(lhs, numDims, numSymbols, operands);
  simplifyExprAndOperands(rhs, numDims, numSymbols, operands);
  expr = getAffineBinaryOpExpr(binExpr.getKind(), lhs, rhs);

  binExpr = expr.dyn_cast<AffineBinaryOpExpr>();
  if (!binExpr || (expr.getKind() != AffineExprKind::FloorDiv &&
                   expr.getKind() != AffineExprKind::CeilDiv &&
                   expr.getKind() != AffineExprKind::Mod)) {
    return;
  }

  // The `lhs` and `rhs` may be different post construction of simplified expr.
  lhs = binExpr.getLHS();
  rhs = binExpr.getRHS();
  auto rhsConst = rhs.dyn_cast<AffineConstantExpr>();
  if (!rhsConst)
    return;

  int64_t rhsConstVal = rhsConst.getValue();
  // Undefined exprsessions aren't touched; IR can still be valid with them.
  if (rhsConstVal <= 0)
    return;

  // Exploit constant lower/upper bounds to simplify a floordiv or mod.
  MLIRContext *context = expr.getContext();
  std::optional<int64_t> lhsLbConst =
      getLowerBound(lhs, numDims, numSymbols, operands);
  std::optional<int64_t> lhsUbConst =
      getUpperBound(lhs, numDims, numSymbols, operands);
  if (lhsLbConst && lhsUbConst) {
    int64_t lhsLbConstVal = *lhsLbConst;
    int64_t lhsUbConstVal = *lhsUbConst;
    // lhs floordiv c is a single value lhs is bounded in a range `c` that has
    // the same quotient.
    if (binExpr.getKind() == AffineExprKind::FloorDiv &&
        floorDiv(lhsLbConstVal, rhsConstVal) ==
            floorDiv(lhsUbConstVal, rhsConstVal)) {
      expr =
          getAffineConstantExpr(floorDiv(lhsLbConstVal, rhsConstVal), context);
      return;
    }
    // lhs ceildiv c is a single value if the entire range has the same ceil
    // quotient.
    if (binExpr.getKind() == AffineExprKind::CeilDiv &&
        ceilDiv(lhsLbConstVal, rhsConstVal) ==
            ceilDiv(lhsUbConstVal, rhsConstVal)) {
      expr =
          getAffineConstantExpr(ceilDiv(lhsLbConstVal, rhsConstVal), context);
      return;
    }
    // lhs mod c is lhs if the entire range has quotient 0 w.r.t the rhs.
    if (binExpr.getKind() == AffineExprKind::Mod && lhsLbConstVal >= 0 &&
        lhsLbConstVal < rhsConstVal && lhsUbConstVal < rhsConstVal) {
      expr = lhs;
      return;
    }
  }

  // Simplify expressions of the form e = (e_1 + e_2) floordiv c or (e_1 + e_2)
  // mod c, where e_1 is a multiple of `k` and 0 <= e_2 < k. In such cases, if
  // `c` % `k` == 0, (e_1 + e_2) floordiv c can be simplified to e_1 floordiv c.
  // And when k % c == 0, (e_1 + e_2) mod c can be simplified to e_2 mod c.
  AffineExpr quotientTimesDiv, rem;
  int64_t divisor;
  if (isQTimesDPlusR(lhs, operands, divisor, quotientTimesDiv, rem)) {
    if (rhsConstVal % divisor == 0 &&
        binExpr.getKind() == AffineExprKind::FloorDiv) {
      expr = quotientTimesDiv.floorDiv(rhsConst);
    } else if (divisor % rhsConstVal == 0 &&
               binExpr.getKind() == AffineExprKind::Mod) {
      expr = rem % rhsConst;
    }
    return;
  }

  // Handle the simple case when the LHS expression can be either upper
  // bounded or is a known multiple of RHS constant.
  // lhs floordiv c -> 0 if 0 <= lhs < c,
  // lhs mod c -> 0 if lhs % c = 0.
  if ((isNonNegativeBoundedBy(lhs, operands, rhsConstVal) &&
       binExpr.getKind() == AffineExprKind::FloorDiv) ||
      (getLargestKnownDivisor(lhs, operands) % rhsConstVal == 0 &&
       binExpr.getKind() == AffineExprKind::Mod)) {
    expr = getAffineConstantExpr(0, expr.getContext());
  }
}

/// Simplify the expressions in `map` while making use of lower or upper bounds
/// of its operands. If `isMax` is true, the map is to be treated as a max of
/// its result expressions, and min otherwise. Eg: min (d0, d1) -> (8, 4 * d0 +
/// d1) can be simplified to (8) if the operands are respectively lower bounded
/// by 2 and 0 (the second expression can't be lower than 8).
static void simplifyMinOrMaxExprWithOperands(AffineMap &map,
                                             ArrayRef<Value> operands,
                                             bool isMax) {
  // Can't simplify.
  if (operands.empty())
    return;

  // Get the upper or lower bound on an affine.for op IV using its range.
  // Get the constant lower or upper bounds on the operands.
  SmallVector<std::optional<int64_t>> constLowerBounds, constUpperBounds;
  constLowerBounds.reserve(operands.size());
  constUpperBounds.reserve(operands.size());
  for (Value operand : operands) {
    constLowerBounds.push_back(getLowerBound(operand));
    constUpperBounds.push_back(getUpperBound(operand));
  }

  // We will compute the lower and upper bounds on each of the expressions
  // Then, we will check (depending on max or min) as to whether a specific
  // bound is redundant by checking if its highest (in case of max) and its
  // lowest (in the case of min) value is already lower than (or higher than)
  // the lower bound (or upper bound in the case of min) of another bound.
  SmallVector<std::optional<int64_t>, 4> lowerBounds, upperBounds;
  lowerBounds.reserve(map.getNumResults());
  upperBounds.reserve(map.getNumResults());
  for (AffineExpr e : map.getResults()) {
    if (auto constExpr = e.dyn_cast<AffineConstantExpr>()) {
      lowerBounds.push_back(constExpr.getValue());
      upperBounds.push_back(constExpr.getValue());
    } else {
      lowerBounds.push_back(getBoundForExpr(e, map.getNumDims(),
                                            map.getNumSymbols(),
                                            constLowerBounds, constUpperBounds,
                                            /*isUpper=*/false));
      upperBounds.push_back(getBoundForExpr(e, map.getNumDims(),
                                            map.getNumSymbols(),
                                            constLowerBounds, constUpperBounds,
                                            /*isUpper=*/true));
    }
  }

  // Collect expressions that are not redundant.
  SmallVector<AffineExpr, 4> irredundantExprs;
  for (auto exprEn : llvm::enumerate(map.getResults())) {
    AffineExpr e = exprEn.value();
    unsigned i = exprEn.index();
    // Some expressions can be turned into constants.
    if (lowerBounds[i] && upperBounds[i] && *lowerBounds[i] == *upperBounds[i])
      e = getAffineConstantExpr(*lowerBounds[i], e.getContext());

    // Check if the expression is redundant.
    if (isMax) {
      if (!upperBounds[i]) {
        irredundantExprs.push_back(e);
        continue;
      }
      // If there exists another expression such that its lower bound is greater
      // than this expression's upper bound, it's redundant.
      if (!llvm::any_of(llvm::enumerate(lowerBounds), [&](const auto &en) {
            auto otherLowerBound = en.value();
            unsigned pos = en.index();
            if (pos == i || !otherLowerBound)
              return false;
            if (*otherLowerBound > *upperBounds[i])
              return true;
            if (*otherLowerBound < *upperBounds[i])
              return false;
            // Equality case. When both expressions are considered redundant, we
            // don't want to get both of them. We keep the one that appears
            // first.
            if (upperBounds[pos] && lowerBounds[i] &&
                lowerBounds[i] == upperBounds[i] &&
                otherLowerBound == *upperBounds[pos] && i < pos)
              return false;
            return true;
          }))
        irredundantExprs.push_back(e);
    } else {
      if (!lowerBounds[i]) {
        irredundantExprs.push_back(e);
        continue;
      }
      // Likewise for the `min` case. Use the complement of the condition above.
      if (!llvm::any_of(llvm::enumerate(upperBounds), [&](const auto &en) {
            auto otherUpperBound = en.value();
            unsigned pos = en.index();
            if (pos == i || !otherUpperBound)
              return false;
            if (*otherUpperBound < *lowerBounds[i])
              return true;
            if (*otherUpperBound > *lowerBounds[i])
              return false;
            if (lowerBounds[pos] && upperBounds[i] &&
                lowerBounds[i] == upperBounds[i] &&
                otherUpperBound == lowerBounds[pos] && i < pos)
              return false;
            return true;
          }))
        irredundantExprs.push_back(e);
    }
  }

  // Create the map without the redundant expressions.
  map = AffineMap::get(map.getNumDims(), map.getNumSymbols(), irredundantExprs,
                       map.getContext());
}

/// Simplify the map while exploiting information on the values in `operands`.
//  Use "unused attribute" marker to silence warning stemming from the inability
//  to see through the template expansion.
static void LLVM_ATTRIBUTE_UNUSED
simplifyMapWithOperands(AffineMap &map, ArrayRef<Value> operands) {
  assert(map.getNumInputs() == operands.size() && "invalid operands for map");
  SmallVector<AffineExpr> newResults;
  newResults.reserve(map.getNumResults());
  for (AffineExpr expr : map.getResults()) {
    simplifyExprAndOperands(expr, map.getNumDims(), map.getNumSymbols(),
                            operands);
    newResults.push_back(expr);
  }
  map = AffineMap::get(map.getNumDims(), map.getNumSymbols(), newResults,
                       map.getContext());
}

/// Replace all occurrences of AffineExpr at position `pos` in `map` by the
/// defining AffineApplyOp expression and operands.
/// When `dimOrSymbolPosition < dims.size()`, AffineDimExpr@[pos] is replaced.
/// When `dimOrSymbolPosition >= dims.size()`,
/// AffineSymbolExpr@[pos - dims.size()] is replaced.
/// Mutate `map`,`dims` and `syms` in place as follows:
///   1. `dims` and `syms` are only appended to.
///   2. `map` dim and symbols are gradually shifted to higher positions.
///   3. Old `dim` and `sym` entries are replaced by nullptr
/// This avoids the need for any bookkeeping.
static LogicalResult replaceDimOrSym(AffineMap *map,
                                     unsigned dimOrSymbolPosition,
                                     SmallVectorImpl<Value> &dims,
                                     SmallVectorImpl<Value> &syms) {
  MLIRContext *ctx = map->getContext();
  bool isDimReplacement = (dimOrSymbolPosition < dims.size());
  unsigned pos = isDimReplacement ? dimOrSymbolPosition
                                  : dimOrSymbolPosition - dims.size();
  Value &v = isDimReplacement ? dims[pos] : syms[pos];
  if (!v)
    return failure();

  auto affineApply = v.getDefiningOp<AffineApplyOp>();
  if (!affineApply)
    return failure();

  // At this point we will perform a replacement of `v`, set the entry in `dim`
  // or `sym` to nullptr immediately.
  v = nullptr;

  // Compute the map, dims and symbols coming from the AffineApplyOp.
  AffineMap composeMap = affineApply.getAffineMap();
  assert(composeMap.getNumResults() == 1 && "affine.apply with >1 results");
  SmallVector<Value> composeOperands(affineApply.getMapOperands().begin(),
                                     affineApply.getMapOperands().end());
  // Canonicalize the map to promote dims to symbols when possible. This is to
  // avoid generating invalid maps.
  canonicalizeMapAndOperands(&composeMap, &composeOperands);
  AffineExpr replacementExpr =
      composeMap.shiftDims(dims.size()).shiftSymbols(syms.size()).getResult(0);
  ValueRange composeDims =
      ArrayRef<Value>(composeOperands).take_front(composeMap.getNumDims());
  ValueRange composeSyms =
      ArrayRef<Value>(composeOperands).take_back(composeMap.getNumSymbols());
  AffineExpr toReplace = isDimReplacement ? getAffineDimExpr(pos, ctx)
                                          : getAffineSymbolExpr(pos, ctx);

  // Append the dims and symbols where relevant and perform the replacement.
  dims.append(composeDims.begin(), composeDims.end());
  syms.append(composeSyms.begin(), composeSyms.end());
  *map = map->replace(toReplace, replacementExpr, dims.size(), syms.size());

  return success();
}

/// Iterate over `operands` and fold away all those produced by an AffineApplyOp
/// iteratively. Perform canonicalization of map and operands as well as
/// AffineMap simplification. `map` and `operands` are mutated in place.
static void composeAffineMapAndOperands(AffineMap *map,
                                        SmallVectorImpl<Value> *operands) {
  if (map->getNumResults() == 0) {
    canonicalizeMapAndOperands(map, operands);
    *map = simplifyAffineMap(*map);
    return;
  }

  MLIRContext *ctx = map->getContext();
  SmallVector<Value, 4> dims(operands->begin(),
                             operands->begin() + map->getNumDims());
  SmallVector<Value, 4> syms(operands->begin() + map->getNumDims(),
                             operands->end());

  // Iterate over dims and symbols coming from AffineApplyOp and replace until
  // exhaustion. This iteratively mutates `map`, `dims` and `syms`. Both `dims`
  // and `syms` can only increase by construction.
  // The implementation uses a `while` loop to support the case of symbols
  // that may be constructed from dims ;this may be overkill.
  while (true) {
    bool changed = false;
    for (unsigned pos = 0; pos != dims.size() + syms.size(); ++pos)
      if ((changed |= succeeded(replaceDimOrSym(map, pos, dims, syms))))
        break;
    if (!changed)
      break;
  }

  // Clear operands so we can fill them anew.
  operands->clear();

  // At this point we may have introduced null operands, prune them out before
  // canonicalizing map and operands.
  unsigned nDims = 0, nSyms = 0;
  SmallVector<AffineExpr, 4> dimReplacements, symReplacements;
  dimReplacements.reserve(dims.size());
  symReplacements.reserve(syms.size());
  for (auto *container : {&dims, &syms}) {
    bool isDim = (container == &dims);
    auto &repls = isDim ? dimReplacements : symReplacements;
    for (const auto &en : llvm::enumerate(*container)) {
      Value v = en.value();
      if (!v) {
        assert(isDim ? !map->isFunctionOfDim(en.index())
                     : !map->isFunctionOfSymbol(en.index()) &&
                           "map is function of unexpected expr@pos");
        repls.push_back(getAffineConstantExpr(0, ctx));
        continue;
      }
      repls.push_back(isDim ? getAffineDimExpr(nDims++, ctx)
                            : getAffineSymbolExpr(nSyms++, ctx));
      operands->push_back(v);
    }
  }
  *map = map->replaceDimsAndSymbols(dimReplacements, symReplacements, nDims,
                                    nSyms);

  // Canonicalize and simplify before returning.
  canonicalizeMapAndOperands(map, operands);
  *map = simplifyAffineMap(*map);
}

void mlir::affine::fullyComposeAffineMapAndOperands(
    AffineMap *map, SmallVectorImpl<Value> *operands) {
  while (llvm::any_of(*operands, [](Value v) {
    return isa_and_nonnull<AffineApplyOp>(v.getDefiningOp());
  })) {
    composeAffineMapAndOperands(map, operands);
  }
}

/// Given a list of `OpFoldResult`, build the necessary operations to populate
/// `actualValues` with values produced by operations. In particular, for any
/// attribute-typed element in `values`, call the constant materializer
/// associated with the Affine dialect to produce an operation. Do NOT notify
/// the builder listener about the constant ops being created as they are
/// intended to be removed after being folded into affine constructs; this is
/// not suitable for use beyond the Affine dialect.
static void materializeConstants(OpBuilder &b, Location loc,
                                 ArrayRef<OpFoldResult> values,
                                 SmallVectorImpl<Operation *> &constants,
                                 SmallVectorImpl<Value> &actualValues) {
  OpBuilder::Listener *listener = b.getListener();
  b.setListener(nullptr);
  auto listenerResetter =
      llvm::make_scope_exit([listener, &b] { b.setListener(listener); });

  actualValues.reserve(values.size());
  auto *dialect = b.getContext()->getLoadedDialect<AffineDialect>();
  for (OpFoldResult ofr : values) {
    if (auto value = llvm::dyn_cast_if_present<Value>(ofr)) {
      actualValues.push_back(value);
      continue;
    }
    // Since we are directly specifying `index` as the result type, we need to
    // ensure the provided attribute is also an index type. Otherwise, the
    // AffineDialect materializer will create invalid `arith.constant`
    // operations if the provided Attribute is any other kind of integer.
    constants.push_back(dialect->materializeConstant(
        b,
        b.getIndexAttr(llvm::cast<IntegerAttr>(ofr.get<Attribute>()).getInt()),
        b.getIndexType(), loc));
    actualValues.push_back(constants.back()->getResult(0));
  }
}

/// Create an operation of the type provided as template argument and attempt to
/// fold it immediately. The operation is expected to have a builder taking
/// arbitrary `leadingArguments`, followed by a list of Value-typed `operands`.
/// The operation is also expected to always produce a single result. Return an
/// `OpFoldResult` containing the Attribute representing the folded constant if
/// complete folding was possible and a Value produced by the created operation
/// otherwise.
template <typename OpTy, typename... Args>
static std::enable_if_t<OpTy::template hasTrait<OpTrait::OneResult>(),
                        OpFoldResult>
createOrFold(OpBuilder &b, Location loc, ValueRange operands,
             Args &&...leadingArguments) {
  // Identify the constant operands and extract their values as attributes.
  // Note that we cannot use the original values directly because the list of
  // operands may have changed due to canonicalization and composition.
  SmallVector<Attribute> constantOperands;
  constantOperands.reserve(operands.size());
  for (Value operand : operands) {
    IntegerAttr attr;
    if (matchPattern(operand, m_Constant(&attr)))
      constantOperands.push_back(attr);
    else
      constantOperands.push_back(nullptr);
  }

  // Create the operation and immediately attempt to fold it. On success,
  // delete the operation and prepare the (unmaterialized) value for being
  // returned. On failure, return the operation result value. Temporarily remove
  // the listener to avoid notifying it when the op is created as it may be
  // removed immediately and there is no way of notifying the caller about that
  // without resorting to RewriterBase.
  //
  // TODO: arguably, the main folder (createOrFold) API should support this use
  // case instead of indiscriminately materializing constants.
  OpBuilder::Listener *listener = b.getListener();
  b.setListener(nullptr);
  auto listenerResetter =
      llvm::make_scope_exit([listener, &b] { b.setListener(listener); });
  OpTy op =
      b.create<OpTy>(loc, std::forward<Args>(leadingArguments)..., operands);
  SmallVector<OpFoldResult, 1> foldResults;
  if (succeeded(op->fold(constantOperands, foldResults)) &&
      !foldResults.empty()) {
    op->erase();
    return foldResults.front();
  }

  // Notify the listener now that we definitely know that the operation will
  // persist. Use the original listener stored in the variable.
  if (listener)
    listener->notifyOperationInserted(op);
  return op->getResult(0);
}

AffineApplyOp mlir::affine::makeComposedAffineApply(OpBuilder &b, Location loc,
                                                    AffineMap map,
                                                    ValueRange operands) {
  AffineMap normalizedMap = map;
  SmallVector<Value, 8> normalizedOperands(operands.begin(), operands.end());
  composeAffineMapAndOperands(&normalizedMap, &normalizedOperands);
  assert(normalizedMap);
  return b.create<AffineApplyOp>(loc, normalizedMap, normalizedOperands);
}

AffineApplyOp mlir::affine::makeComposedAffineApply(OpBuilder &b, Location loc,
                                                    AffineExpr e,
                                                    ValueRange values) {
  return makeComposedAffineApply(
      b, loc, AffineMap::inferFromExprList(ArrayRef<AffineExpr>{e}).front(),
      values);
}

/// Composes the given affine map with the given list of operands, pulling in
/// the maps from any affine.apply operations that supply the operands.
static void composeMultiResultAffineMap(AffineMap &map,
                                        SmallVectorImpl<Value> &operands) {
  // Compose and canonicalize each expression in the map individually because
  // composition only applies to single-result maps, collecting potentially
  // duplicate operands in a single list with shifted dimensions and symbols.
  SmallVector<Value> dims, symbols;
  SmallVector<AffineExpr> exprs;
  for (unsigned i : llvm::seq<unsigned>(0, map.getNumResults())) {
    SmallVector<Value> submapOperands(operands.begin(), operands.end());
    AffineMap submap = map.getSubMap({i});
    fullyComposeAffineMapAndOperands(&submap, &submapOperands);
    canonicalizeMapAndOperands(&submap, &submapOperands);
    unsigned numNewDims = submap.getNumDims();
    submap = submap.shiftDims(dims.size()).shiftSymbols(symbols.size());
    llvm::append_range(dims,
                       ArrayRef<Value>(submapOperands).take_front(numNewDims));
    llvm::append_range(symbols,
                       ArrayRef<Value>(submapOperands).drop_front(numNewDims));
    exprs.push_back(submap.getResult(0));
  }

  // Canonicalize the map created from composed expressions to deduplicate the
  // dimension and symbol operands.
  operands = llvm::to_vector(llvm::concat<Value>(dims, symbols));
  map = AffineMap::get(dims.size(), symbols.size(), exprs, map.getContext());
  canonicalizeMapAndOperands(&map, &operands);
}

OpFoldResult
mlir::affine::makeComposedFoldedAffineApply(OpBuilder &b, Location loc,
                                            AffineMap map,
                                            ArrayRef<OpFoldResult> operands) {
  assert(map.getNumResults() == 1 && "building affine.apply with !=1 result");

  SmallVector<Operation *> constants;
  SmallVector<Value> actualValues;
  materializeConstants(b, loc, operands, constants, actualValues);
  composeAffineMapAndOperands(&map, &actualValues);
  OpFoldResult result = createOrFold<AffineApplyOp>(b, loc, actualValues, map);

  // Constants are always folded into affine min/max because they can be
  // represented as constant expressions, so delete them.
  for (Operation *op : constants)
    op->erase();
  return result;
}

OpFoldResult
mlir::affine::makeComposedFoldedAffineApply(OpBuilder &b, Location loc,
                                            AffineExpr expr,
                                            ArrayRef<OpFoldResult> operands) {
  return makeComposedFoldedAffineApply(
      b, loc, AffineMap::inferFromExprList(ArrayRef<AffineExpr>{expr}).front(),
      operands);
}

SmallVector<OpFoldResult>
mlir::affine::makeComposedFoldedMultiResultAffineApply(
    OpBuilder &b, Location loc, AffineMap map,
    ArrayRef<OpFoldResult> operands) {
  return llvm::map_to_vector(llvm::seq<unsigned>(0, map.getNumResults()),
                             [&](unsigned i) {
                               return makeComposedFoldedAffineApply(
                                   b, loc, map.getSubMap({i}), operands);
                             });
}

Value mlir::affine::makeComposedAffineMin(OpBuilder &b, Location loc,
                                          AffineMap map, ValueRange operands) {
  SmallVector<Value> allOperands = llvm::to_vector(operands);
  composeMultiResultAffineMap(map, allOperands);
  return b.createOrFold<AffineMinOp>(loc, b.getIndexType(), map, allOperands);
}

template <typename OpTy>
static OpFoldResult makeComposedFoldedMinMax(OpBuilder &b, Location loc,
                                             AffineMap map,
                                             ArrayRef<OpFoldResult> operands) {
  SmallVector<Operation *> constants;
  SmallVector<Value> actualValues;
  materializeConstants(b, loc, operands, constants, actualValues);
  composeMultiResultAffineMap(map, actualValues);
  OpFoldResult result =
      createOrFold<OpTy>(b, loc, actualValues, b.getIndexType(), map);

  // Constants are always folded into affine min/max because they can be
  // represented as constant expressions, so delete them.
  for (Operation *op : constants)
    op->erase();
  return result;
}

OpFoldResult
mlir::affine::makeComposedFoldedAffineMin(OpBuilder &b, Location loc,
                                          AffineMap map,
                                          ArrayRef<OpFoldResult> operands) {
  return makeComposedFoldedMinMax<AffineMinOp>(b, loc, map, operands);
}

OpFoldResult
mlir::affine::makeComposedFoldedAffineMax(OpBuilder &b, Location loc,
                                          AffineMap map,
                                          ArrayRef<OpFoldResult> operands) {
  return makeComposedFoldedMinMax<AffineMaxOp>(b, loc, map, operands);
}

/// Fully compose map with operands and canonicalize the result.
/// Return the `createOrFold`'ed AffineApply op.
static Value createFoldedComposedAffineApply(OpBuilder &b, Location loc,
                                             AffineMap map,
                                             ValueRange operandsRef) {
  SmallVector<Value, 4> operands(operandsRef.begin(), operandsRef.end());
  fullyComposeAffineMapAndOperands(&map, &operands);
  canonicalizeMapAndOperands(&map, &operands);
  return b.createOrFold<AffineApplyOp>(loc, map, operands);
}

SmallVector<Value, 4> mlir::affine::applyMapToValues(OpBuilder &b, Location loc,
                                                     AffineMap map,
                                                     ValueRange values) {
  SmallVector<Value, 4> res;
  res.reserve(map.getNumResults());
  unsigned numDims = map.getNumDims(), numSym = map.getNumSymbols();
  // For each `expr` in `map`, applies the `expr` to the values extracted from
  // ranges. If the resulting application can be folded into a Value, the
  // folding occurs eagerly.
  for (auto expr : map.getResults()) {
    AffineMap map = AffineMap::get(numDims, numSym, expr);
    res.push_back(createFoldedComposedAffineApply(b, loc, map, values));
  }
  return res;
}

// A symbol may appear as a dim in affine.apply operations. This function
// canonicalizes dims that are valid symbols into actual symbols.
template <class MapOrSet>
static void canonicalizePromotedSymbols(MapOrSet *mapOrSet,
                                        SmallVectorImpl<Value> *operands) {
  if (!mapOrSet || operands->empty())
    return;

  assert(mapOrSet->getNumInputs() == operands->size() &&
         "map/set inputs must match number of operands");

  auto *context = mapOrSet->getContext();
  SmallVector<Value, 8> resultOperands;
  resultOperands.reserve(operands->size());
  SmallVector<Value, 8> remappedSymbols;
  remappedSymbols.reserve(operands->size());
  unsigned nextDim = 0;
  unsigned nextSym = 0;
  unsigned oldNumSyms = mapOrSet->getNumSymbols();
  SmallVector<AffineExpr, 8> dimRemapping(mapOrSet->getNumDims());
  for (unsigned i = 0, e = mapOrSet->getNumInputs(); i != e; ++i) {
    if (i < mapOrSet->getNumDims()) {
      if (isValidSymbol((*operands)[i])) {
        // This is a valid symbol that appears as a dim, canonicalize it.
        dimRemapping[i] = getAffineSymbolExpr(oldNumSyms + nextSym++, context);
        remappedSymbols.push_back((*operands)[i]);
      } else {
        dimRemapping[i] = getAffineDimExpr(nextDim++, context);
        resultOperands.push_back((*operands)[i]);
      }
    } else {
      resultOperands.push_back((*operands)[i]);
    }
  }

  resultOperands.append(remappedSymbols.begin(), remappedSymbols.end());
  *operands = resultOperands;
  *mapOrSet = mapOrSet->replaceDimsAndSymbols(dimRemapping, {}, nextDim,
                                              oldNumSyms + nextSym);

  assert(mapOrSet->getNumInputs() == operands->size() &&
         "map/set inputs must match number of operands");
}

// Works for either an affine map or an integer set.
template <class MapOrSet>
static void canonicalizeMapOrSetAndOperands(MapOrSet *mapOrSet,
                                            SmallVectorImpl<Value> *operands) {
  static_assert(llvm::is_one_of<MapOrSet, AffineMap, IntegerSet>::value,
                "Argument must be either of AffineMap or IntegerSet type");

  if (!mapOrSet || operands->empty())
    return;

  assert(mapOrSet->getNumInputs() == operands->size() &&
         "map/set inputs must match number of operands");

  canonicalizePromotedSymbols<MapOrSet>(mapOrSet, operands);

  // Check to see what dims are used.
  llvm::SmallBitVector usedDims(mapOrSet->getNumDims());
  llvm::SmallBitVector usedSyms(mapOrSet->getNumSymbols());
  mapOrSet->walkExprs([&](AffineExpr expr) {
    if (auto dimExpr = expr.dyn_cast<AffineDimExpr>())
      usedDims[dimExpr.getPosition()] = true;
    else if (auto symExpr = expr.dyn_cast<AffineSymbolExpr>())
      usedSyms[symExpr.getPosition()] = true;
  });

  auto *context = mapOrSet->getContext();

  SmallVector<Value, 8> resultOperands;
  resultOperands.reserve(operands->size());

  llvm::SmallDenseMap<Value, AffineExpr, 8> seenDims;
  SmallVector<AffineExpr, 8> dimRemapping(mapOrSet->getNumDims());
  unsigned nextDim = 0;
  for (unsigned i = 0, e = mapOrSet->getNumDims(); i != e; ++i) {
    if (usedDims[i]) {
      // Remap dim positions for duplicate operands.
      auto it = seenDims.find((*operands)[i]);
      if (it == seenDims.end()) {
        dimRemapping[i] = getAffineDimExpr(nextDim++, context);
        resultOperands.push_back((*operands)[i]);
        seenDims.insert(std::make_pair((*operands)[i], dimRemapping[i]));
      } else {
        dimRemapping[i] = it->second;
      }
    }
  }
  llvm::SmallDenseMap<Value, AffineExpr, 8> seenSymbols;
  SmallVector<AffineExpr, 8> symRemapping(mapOrSet->getNumSymbols());
  unsigned nextSym = 0;
  for (unsigned i = 0, e = mapOrSet->getNumSymbols(); i != e; ++i) {
    if (!usedSyms[i])
      continue;
    // Handle constant operands (only needed for symbolic operands since
    // constant operands in dimensional positions would have already been
    // promoted to symbolic positions above).
    IntegerAttr operandCst;
    if (matchPattern((*operands)[i + mapOrSet->getNumDims()],
                     m_Constant(&operandCst))) {
      symRemapping[i] =
          getAffineConstantExpr(operandCst.getValue().getSExtValue(), context);
      continue;
    }
    // Remap symbol positions for duplicate operands.
    auto it = seenSymbols.find((*operands)[i + mapOrSet->getNumDims()]);
    if (it == seenSymbols.end()) {
      symRemapping[i] = getAffineSymbolExpr(nextSym++, context);
      resultOperands.push_back((*operands)[i + mapOrSet->getNumDims()]);
      seenSymbols.insert(std::make_pair((*operands)[i + mapOrSet->getNumDims()],
                                        symRemapping[i]));
    } else {
      symRemapping[i] = it->second;
    }
  }
  *mapOrSet = mapOrSet->replaceDimsAndSymbols(dimRemapping, symRemapping,
                                              nextDim, nextSym);
  *operands = resultOperands;
}

void mlir::affine::canonicalizeMapAndOperands(
    AffineMap *map, SmallVectorImpl<Value> *operands) {
  canonicalizeMapOrSetAndOperands<AffineMap>(map, operands);
}

void mlir::affine::canonicalizeSetAndOperands(
    IntegerSet *set, SmallVectorImpl<Value> *operands) {
  canonicalizeMapOrSetAndOperands<IntegerSet>(set, operands);
}

namespace {
/// Simplify AffineApply, AffineLoad, and AffineStore operations by composing
/// maps that supply results into them.
///
template <typename AffineOpTy>
struct SimplifyAffineOp : public OpRewritePattern<AffineOpTy> {
  using OpRewritePattern<AffineOpTy>::OpRewritePattern;

  /// Replace the affine op with another instance of it with the supplied
  /// map and mapOperands.
  void replaceAffineOp(PatternRewriter &rewriter, AffineOpTy affineOp,
                       AffineMap map, ArrayRef<Value> mapOperands) const;

  LogicalResult matchAndRewrite(AffineOpTy affineOp,
                                PatternRewriter &rewriter) const override {
    static_assert(
        llvm::is_one_of<AffineOpTy, AffineLoadOp, AffinePrefetchOp,
                        AffineStoreOp, AffineApplyOp, AffineMinOp, AffineMaxOp,
                        AffineVectorStoreOp, AffineVectorLoadOp>::value,
        "affine load/store/vectorstore/vectorload/apply/prefetch/min/max op "
        "expected");
    auto map = affineOp.getAffineMap();
    AffineMap oldMap = map;
    auto oldOperands = affineOp.getMapOperands();
    SmallVector<Value, 8> resultOperands(oldOperands);
    composeAffineMapAndOperands(&map, &resultOperands);
    canonicalizeMapAndOperands(&map, &resultOperands);
    simplifyMapWithOperands(map, resultOperands);
    if (map == oldMap && std::equal(oldOperands.begin(), oldOperands.end(),
                                    resultOperands.begin()))
      return failure();

    replaceAffineOp(rewriter, affineOp, map, resultOperands);
    return success();
  }
};

// Specialize the template to account for the different build signatures for
// affine load, store, and apply ops.
template <>
void SimplifyAffineOp<AffineLoadOp>::replaceAffineOp(
    PatternRewriter &rewriter, AffineLoadOp load, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<AffineLoadOp>(load, load.getMemRef(), map,
                                            mapOperands);
}
template <>
void SimplifyAffineOp<AffinePrefetchOp>::replaceAffineOp(
    PatternRewriter &rewriter, AffinePrefetchOp prefetch, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<AffinePrefetchOp>(
      prefetch, prefetch.getMemref(), map, mapOperands,
      prefetch.getLocalityHint(), prefetch.getIsWrite(),
      prefetch.getIsDataCache());
}
template <>
void SimplifyAffineOp<AffineStoreOp>::replaceAffineOp(
    PatternRewriter &rewriter, AffineStoreOp store, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<AffineStoreOp>(
      store, store.getValueToStore(), store.getMemRef(), map, mapOperands);
}
template <>
void SimplifyAffineOp<AffineVectorLoadOp>::replaceAffineOp(
    PatternRewriter &rewriter, AffineVectorLoadOp vectorload, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<AffineVectorLoadOp>(
      vectorload, vectorload.getVectorType(), vectorload.getMemRef(), map,
      mapOperands);
}
template <>
void SimplifyAffineOp<AffineVectorStoreOp>::replaceAffineOp(
    PatternRewriter &rewriter, AffineVectorStoreOp vectorstore, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<AffineVectorStoreOp>(
      vectorstore, vectorstore.getValueToStore(), vectorstore.getMemRef(), map,
      mapOperands);
}

// Generic version for ops that don't have extra operands.
template <typename AffineOpTy>
void SimplifyAffineOp<AffineOpTy>::replaceAffineOp(
    PatternRewriter &rewriter, AffineOpTy op, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<AffineOpTy>(op, map, mapOperands);
}
} // namespace

void AffineApplyOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                MLIRContext *context) {
  results.add<SimplifyAffineOp<AffineApplyOp>>(context);
}

//===----------------------------------------------------------------------===//
// AffineDmaStartOp
//===----------------------------------------------------------------------===//

// TODO: Check that map operands are loop IVs or symbols.
void AffineDmaStartOp::build(OpBuilder &builder, OperationState &result,
                             Value srcMemRef, AffineMap srcMap,
                             ValueRange srcIndices, Value destMemRef,
                             AffineMap dstMap, ValueRange destIndices,
                             Value tagMemRef, AffineMap tagMap,
                             ValueRange tagIndices, Value numElements,
                             Value stride, Value elementsPerStride) {
  result.addOperands(srcMemRef);
  result.addAttribute(getSrcMapAttrStrName(), AffineMapAttr::get(srcMap));
  result.addOperands(srcIndices);
  result.addOperands(destMemRef);
  result.addAttribute(getDstMapAttrStrName(), AffineMapAttr::get(dstMap));
  result.addOperands(destIndices);
  result.addOperands(tagMemRef);
  result.addAttribute(getTagMapAttrStrName(), AffineMapAttr::get(tagMap));
  result.addOperands(tagIndices);
  result.addOperands(numElements);
  if (stride) {
    result.addOperands({stride, elementsPerStride});
  }
}

void AffineDmaStartOp::print(OpAsmPrinter &p) {
  p << " " << getSrcMemRef() << '[';
  p.printAffineMapOfSSAIds(getSrcMapAttr(), getSrcIndices());
  p << "], " << getDstMemRef() << '[';
  p.printAffineMapOfSSAIds(getDstMapAttr(), getDstIndices());
  p << "], " << getTagMemRef() << '[';
  p.printAffineMapOfSSAIds(getTagMapAttr(), getTagIndices());
  p << "], " << getNumElements();
  if (isStrided()) {
    p << ", " << getStride();
    p << ", " << getNumElementsPerStride();
  }
  p << " : " << getSrcMemRefType() << ", " << getDstMemRefType() << ", "
    << getTagMemRefType();
}

// Parse AffineDmaStartOp.
// Ex:
//   affine.dma_start %src[%i, %j], %dst[%k, %l], %tag[%index], %size,
//     %stride, %num_elt_per_stride
//       : memref<3076 x f32, 0>, memref<1024 x f32, 2>, memref<1 x i32>
//
ParseResult AffineDmaStartOp::parse(OpAsmParser &parser,
                                    OperationState &result) {
  OpAsmParser::UnresolvedOperand srcMemRefInfo;
  AffineMapAttr srcMapAttr;
  SmallVector<OpAsmParser::UnresolvedOperand, 4> srcMapOperands;
  OpAsmParser::UnresolvedOperand dstMemRefInfo;
  AffineMapAttr dstMapAttr;
  SmallVector<OpAsmParser::UnresolvedOperand, 4> dstMapOperands;
  OpAsmParser::UnresolvedOperand tagMemRefInfo;
  AffineMapAttr tagMapAttr;
  SmallVector<OpAsmParser::UnresolvedOperand, 4> tagMapOperands;
  OpAsmParser::UnresolvedOperand numElementsInfo;
  SmallVector<OpAsmParser::UnresolvedOperand, 2> strideInfo;

  SmallVector<Type, 3> types;
  auto indexType = parser.getBuilder().getIndexType();

  // Parse and resolve the following list of operands:
  // *) dst memref followed by its affine maps operands (in square brackets).
  // *) src memref followed by its affine map operands (in square brackets).
  // *) tag memref followed by its affine map operands (in square brackets).
  // *) number of elements transferred by DMA operation.
  if (parser.parseOperand(srcMemRefInfo) ||
      parser.parseAffineMapOfSSAIds(srcMapOperands, srcMapAttr,
                                    getSrcMapAttrStrName(),
                                    result.attributes) ||
      parser.parseComma() || parser.parseOperand(dstMemRefInfo) ||
      parser.parseAffineMapOfSSAIds(dstMapOperands, dstMapAttr,
                                    getDstMapAttrStrName(),
                                    result.attributes) ||
      parser.parseComma() || parser.parseOperand(tagMemRefInfo) ||
      parser.parseAffineMapOfSSAIds(tagMapOperands, tagMapAttr,
                                    getTagMapAttrStrName(),
                                    result.attributes) ||
      parser.parseComma() || parser.parseOperand(numElementsInfo))
    return failure();

  // Parse optional stride and elements per stride.
  if (parser.parseTrailingOperandList(strideInfo))
    return failure();

  if (!strideInfo.empty() && strideInfo.size() != 2) {
    return parser.emitError(parser.getNameLoc(),
                            "expected two stride related operands");
  }
  bool isStrided = strideInfo.size() == 2;

  if (parser.parseColonTypeList(types))
    return failure();

  if (types.size() != 3)
    return parser.emitError(parser.getNameLoc(), "expected three types");

  if (parser.resolveOperand(srcMemRefInfo, types[0], result.operands) ||
      parser.resolveOperands(srcMapOperands, indexType, result.operands) ||
      parser.resolveOperand(dstMemRefInfo, types[1], result.operands) ||
      parser.resolveOperands(dstMapOperands, indexType, result.operands) ||
      parser.resolveOperand(tagMemRefInfo, types[2], result.operands) ||
      parser.resolveOperands(tagMapOperands, indexType, result.operands) ||
      parser.resolveOperand(numElementsInfo, indexType, result.operands))
    return failure();

  if (isStrided) {
    if (parser.resolveOperands(strideInfo, indexType, result.operands))
      return failure();
  }

  // Check that src/dst/tag operand counts match their map.numInputs.
  if (srcMapOperands.size() != srcMapAttr.getValue().getNumInputs() ||
      dstMapOperands.size() != dstMapAttr.getValue().getNumInputs() ||
      tagMapOperands.size() != tagMapAttr.getValue().getNumInputs())
    return parser.emitError(parser.getNameLoc(),
                            "memref operand count not equal to map.numInputs");
  return success();
}

LogicalResult AffineDmaStartOp::verifyInvariantsImpl() {
  if (!llvm::isa<MemRefType>(getOperand(getSrcMemRefOperandIndex()).getType()))
    return emitOpError("expected DMA source to be of memref type");
  if (!llvm::isa<MemRefType>(getOperand(getDstMemRefOperandIndex()).getType()))
    return emitOpError("expected DMA destination to be of memref type");
  if (!llvm::isa<MemRefType>(getOperand(getTagMemRefOperandIndex()).getType()))
    return emitOpError("expected DMA tag to be of memref type");

  unsigned numInputsAllMaps = getSrcMap().getNumInputs() +
                              getDstMap().getNumInputs() +
                              getTagMap().getNumInputs();
  if (getNumOperands() != numInputsAllMaps + 3 + 1 &&
      getNumOperands() != numInputsAllMaps + 3 + 1 + 2) {
    return emitOpError("incorrect number of operands");
  }

  Region *scope = getAffineScope(*this);
  for (auto idx : getSrcIndices()) {
    if (!idx.getType().isIndex())
      return emitOpError("src index to dma_start must have 'index' type");
    if (!isValidAffineIndexOperand(idx, scope))
      return emitOpError("src index must be a dimension or symbol identifier");
  }
  for (auto idx : getDstIndices()) {
    if (!idx.getType().isIndex())
      return emitOpError("dst index to dma_start must have 'index' type");
    if (!isValidAffineIndexOperand(idx, scope))
      return emitOpError("dst index must be a dimension or symbol identifier");
  }
  for (auto idx : getTagIndices()) {
    if (!idx.getType().isIndex())
      return emitOpError("tag index to dma_start must have 'index' type");
    if (!isValidAffineIndexOperand(idx, scope))
      return emitOpError("tag index must be a dimension or symbol identifier");
  }
  return success();
}

LogicalResult AffineDmaStartOp::fold(ArrayRef<Attribute> cstOperands,
                                     SmallVectorImpl<OpFoldResult> &results) {
  /// dma_start(memrefcast) -> dma_start
  return memref::foldMemRefCast(*this);
}

void AffineDmaStartOp::getEffects(
    SmallVectorImpl<SideEffects::EffectInstance<MemoryEffects::Effect>>
        &effects) {
  effects.emplace_back(MemoryEffects::Read::get(), getSrcMemRef(),
                       SideEffects::DefaultResource::get());
  effects.emplace_back(MemoryEffects::Write::get(), getDstMemRef(),
                       SideEffects::DefaultResource::get());
  effects.emplace_back(MemoryEffects::Read::get(), getTagMemRef(),
                       SideEffects::DefaultResource::get());
}

//===----------------------------------------------------------------------===//
// AffineDmaWaitOp
//===----------------------------------------------------------------------===//

// TODO: Check that map operands are loop IVs or symbols.
void AffineDmaWaitOp::build(OpBuilder &builder, OperationState &result,
                            Value tagMemRef, AffineMap tagMap,
                            ValueRange tagIndices, Value numElements) {
  result.addOperands(tagMemRef);
  result.addAttribute(getTagMapAttrStrName(), AffineMapAttr::get(tagMap));
  result.addOperands(tagIndices);
  result.addOperands(numElements);
}

void AffineDmaWaitOp::print(OpAsmPrinter &p) {
  p << " " << getTagMemRef() << '[';
  SmallVector<Value, 2> operands(getTagIndices());
  p.printAffineMapOfSSAIds(getTagMapAttr(), operands);
  p << "], ";
  p.printOperand(getNumElements());
  p << " : " << getTagMemRef().getType();
}

// Parse AffineDmaWaitOp.
// Eg:
//   affine.dma_wait %tag[%index], %num_elements
//     : memref<1 x i32, (d0) -> (d0), 4>
//
ParseResult AffineDmaWaitOp::parse(OpAsmParser &parser,
                                   OperationState &result) {
  OpAsmParser::UnresolvedOperand tagMemRefInfo;
  AffineMapAttr tagMapAttr;
  SmallVector<OpAsmParser::UnresolvedOperand, 2> tagMapOperands;
  Type type;
  auto indexType = parser.getBuilder().getIndexType();
  OpAsmParser::UnresolvedOperand numElementsInfo;

  // Parse tag memref, its map operands, and dma size.
  if (parser.parseOperand(tagMemRefInfo) ||
      parser.parseAffineMapOfSSAIds(tagMapOperands, tagMapAttr,
                                    getTagMapAttrStrName(),
                                    result.attributes) ||
      parser.parseComma() || parser.parseOperand(numElementsInfo) ||
      parser.parseColonType(type) ||
      parser.resolveOperand(tagMemRefInfo, type, result.operands) ||
      parser.resolveOperands(tagMapOperands, indexType, result.operands) ||
      parser.resolveOperand(numElementsInfo, indexType, result.operands))
    return failure();

  if (!llvm::isa<MemRefType>(type))
    return parser.emitError(parser.getNameLoc(),
                            "expected tag to be of memref type");

  if (tagMapOperands.size() != tagMapAttr.getValue().getNumInputs())
    return parser.emitError(parser.getNameLoc(),
                            "tag memref operand count != to map.numInputs");
  return success();
}

LogicalResult AffineDmaWaitOp::verifyInvariantsImpl() {
  if (!llvm::isa<MemRefType>(getOperand(0).getType()))
    return emitOpError("expected DMA tag to be of memref type");
  Region *scope = getAffineScope(*this);
  for (auto idx : getTagIndices()) {
    if (!idx.getType().isIndex())
      return emitOpError("index to dma_wait must have 'index' type");
    if (!isValidAffineIndexOperand(idx, scope))
      return emitOpError("index must be a dimension or symbol identifier");
  }
  return success();
}

LogicalResult AffineDmaWaitOp::fold(ArrayRef<Attribute> cstOperands,
                                    SmallVectorImpl<OpFoldResult> &results) {
  /// dma_wait(memrefcast) -> dma_wait
  return memref::foldMemRefCast(*this);
}

void AffineDmaWaitOp::getEffects(
    SmallVectorImpl<SideEffects::EffectInstance<MemoryEffects::Effect>>
        &effects) {
  effects.emplace_back(MemoryEffects::Read::get(), getTagMemRef(),
                       SideEffects::DefaultResource::get());
}

//===----------------------------------------------------------------------===//
// AffineForOp
//===----------------------------------------------------------------------===//

/// 'bodyBuilder' is used to build the body of affine.for. If iterArgs and
/// bodyBuilder are empty/null, we include default terminator op.
void AffineForOp::build(OpBuilder &builder, OperationState &result,
                        ValueRange lbOperands, AffineMap lbMap,
                        ValueRange ubOperands, AffineMap ubMap, int64_t step,
                        ValueRange iterArgs, BodyBuilderFn bodyBuilder) {
  assert(((!lbMap && lbOperands.empty()) ||
          lbOperands.size() == lbMap.getNumInputs()) &&
         "lower bound operand count does not match the affine map");
  assert(((!ubMap && ubOperands.empty()) ||
          ubOperands.size() == ubMap.getNumInputs()) &&
         "upper bound operand count does not match the affine map");
  assert(step > 0 && "step has to be a positive integer constant");

  for (Value val : iterArgs)
    result.addTypes(val.getType());

  // Add an attribute for the step.
  result.addAttribute(getStepAttrStrName(),
                      builder.getIntegerAttr(builder.getIndexType(), step));

  // Add the lower bound.
  result.addAttribute(getLowerBoundAttrStrName(), AffineMapAttr::get(lbMap));
  result.addOperands(lbOperands);

  // Add the upper bound.
  result.addAttribute(getUpperBoundAttrStrName(), AffineMapAttr::get(ubMap));
  result.addOperands(ubOperands);

  result.addOperands(iterArgs);
  // Create a region and a block for the body.  The argument of the region is
  // the loop induction variable.
  Region *bodyRegion = result.addRegion();
  bodyRegion->push_back(new Block);
  Block &bodyBlock = bodyRegion->front();
  Value inductionVar =
      bodyBlock.addArgument(builder.getIndexType(), result.location);
  for (Value val : iterArgs)
    bodyBlock.addArgument(val.getType(), val.getLoc());

  // Create the default terminator if the builder is not provided and if the
  // iteration arguments are not provided. Otherwise, leave this to the caller
  // because we don't know which values to return from the loop.
  if (iterArgs.empty() && !bodyBuilder) {
    ensureTerminator(*bodyRegion, builder, result.location);
  } else if (bodyBuilder) {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToStart(&bodyBlock);
    bodyBuilder(builder, result.location, inductionVar,
                bodyBlock.getArguments().drop_front());
  }
}

void AffineForOp::build(OpBuilder &builder, OperationState &result, int64_t lb,
                        int64_t ub, int64_t step, ValueRange iterArgs,
                        BodyBuilderFn bodyBuilder) {
  auto lbMap = AffineMap::getConstantMap(lb, builder.getContext());
  auto ubMap = AffineMap::getConstantMap(ub, builder.getContext());
  return build(builder, result, {}, lbMap, {}, ubMap, step, iterArgs,
               bodyBuilder);
}

LogicalResult AffineForOp::verifyRegions() {
  // Check that the body defines as single block argument for the induction
  // variable.
  auto *body = getBody();
  if (body->getNumArguments() == 0 || !body->getArgument(0).getType().isIndex())
    return emitOpError("expected body to have a single index argument for the "
                       "induction variable");

  // Verify that the bound operands are valid dimension/symbols.
  /// Lower bound.
  if (getLowerBoundMap().getNumInputs() > 0)
    if (failed(verifyDimAndSymbolIdentifiers(*this, getLowerBoundOperands(),
                                             getLowerBoundMap().getNumDims())))
      return failure();
  /// Upper bound.
  if (getUpperBoundMap().getNumInputs() > 0)
    if (failed(verifyDimAndSymbolIdentifiers(*this, getUpperBoundOperands(),
                                             getUpperBoundMap().getNumDims())))
      return failure();

  unsigned opNumResults = getNumResults();
  if (opNumResults == 0)
    return success();

  // If ForOp defines values, check that the number and types of the defined
  // values match ForOp initial iter operands and backedge basic block
  // arguments.
  if (getNumIterOperands() != opNumResults)
    return emitOpError(
        "mismatch between the number of loop-carried values and results");
  if (getNumRegionIterArgs() != opNumResults)
    return emitOpError(
        "mismatch between the number of basic block args and results");

  return success();
}

/// Parse a for operation loop bounds.
static ParseResult parseBound(bool isLower, OperationState &result,
                              OpAsmParser &p) {
  // 'min' / 'max' prefixes are generally syntactic sugar, but are required if
  // the map has multiple results.
  bool failedToParsedMinMax =
      failed(p.parseOptionalKeyword(isLower ? "max" : "min"));

  auto &builder = p.getBuilder();
  auto boundAttrStrName = isLower ? AffineForOp::getLowerBoundAttrStrName()
                                  : AffineForOp::getUpperBoundAttrStrName();

  // Parse ssa-id as identity map.
  SmallVector<OpAsmParser::UnresolvedOperand, 1> boundOpInfos;
  if (p.parseOperandList(boundOpInfos))
    return failure();

  if (!boundOpInfos.empty()) {
    // Check that only one operand was parsed.
    if (boundOpInfos.size() > 1)
      return p.emitError(p.getNameLoc(),
                         "expected only one loop bound operand");

    // TODO: improve error message when SSA value is not of index type.
    // Currently it is 'use of value ... expects different type than prior uses'
    if (p.resolveOperand(boundOpInfos.front(), builder.getIndexType(),
                         result.operands))
      return failure();

    // Create an identity map using symbol id. This representation is optimized
    // for storage. Analysis passes may expand it into a multi-dimensional map
    // if desired.
    AffineMap map = builder.getSymbolIdentityMap();
    result.addAttribute(boundAttrStrName, AffineMapAttr::get(map));
    return success();
  }

  // Get the attribute location.
  SMLoc attrLoc = p.getCurrentLocation();

  Attribute boundAttr;
  if (p.parseAttribute(boundAttr, builder.getIndexType(), boundAttrStrName,
                       result.attributes))
    return failure();

  // Parse full form - affine map followed by dim and symbol list.
  if (auto affineMapAttr = llvm::dyn_cast<AffineMapAttr>(boundAttr)) {
    unsigned currentNumOperands = result.operands.size();
    unsigned numDims;
    if (parseDimAndSymbolList(p, result.operands, numDims))
      return failure();

    auto map = affineMapAttr.getValue();
    if (map.getNumDims() != numDims)
      return p.emitError(
          p.getNameLoc(),
          "dim operand count and affine map dim count must match");

    unsigned numDimAndSymbolOperands =
        result.operands.size() - currentNumOperands;
    if (numDims + map.getNumSymbols() != numDimAndSymbolOperands)
      return p.emitError(
          p.getNameLoc(),
          "symbol operand count and affine map symbol count must match");

    // If the map has multiple results, make sure that we parsed the min/max
    // prefix.
    if (map.getNumResults() > 1 && failedToParsedMinMax) {
      if (isLower) {
        return p.emitError(attrLoc, "lower loop bound affine map with "
                                    "multiple results requires 'max' prefix");
      }
      return p.emitError(attrLoc, "upper loop bound affine map with multiple "
                                  "results requires 'min' prefix");
    }
    return success();
  }

  // Parse custom assembly form.
  if (auto integerAttr = llvm::dyn_cast<IntegerAttr>(boundAttr)) {
    result.attributes.pop_back();
    result.addAttribute(
        boundAttrStrName,
        AffineMapAttr::get(builder.getConstantAffineMap(integerAttr.getInt())));
    return success();
  }

  return p.emitError(
      p.getNameLoc(),
      "expected valid affine map representation for loop bounds");
}

ParseResult AffineForOp::parse(OpAsmParser &parser, OperationState &result) {
  auto &builder = parser.getBuilder();
  OpAsmParser::Argument inductionVariable;
  inductionVariable.type = builder.getIndexType();
  // Parse the induction variable followed by '='.
  if (parser.parseArgument(inductionVariable) || parser.parseEqual())
    return failure();

  // Parse loop bounds.
  if (parseBound(/*isLower=*/true, result, parser) ||
      parser.parseKeyword("to", " between bounds") ||
      parseBound(/*isLower=*/false, result, parser))
    return failure();

  // Parse the optional loop step, we default to 1 if one is not present.
  if (parser.parseOptionalKeyword("step")) {
    result.addAttribute(
        AffineForOp::getStepAttrStrName(),
        builder.getIntegerAttr(builder.getIndexType(), /*value=*/1));
  } else {
    SMLoc stepLoc = parser.getCurrentLocation();
    IntegerAttr stepAttr;
    if (parser.parseAttribute(stepAttr, builder.getIndexType(),
                              AffineForOp::getStepAttrStrName().data(),
                              result.attributes))
      return failure();

    if (stepAttr.getValue().isNegative())
      return parser.emitError(
          stepLoc,
          "expected step to be representable as a positive signed integer");
  }

  // Parse the optional initial iteration arguments.
  SmallVector<OpAsmParser::Argument, 4> regionArgs;
  SmallVector<OpAsmParser::UnresolvedOperand, 4> operands;

  // Induction variable.
  regionArgs.push_back(inductionVariable);

  if (succeeded(parser.parseOptionalKeyword("iter_args"))) {
    // Parse assignment list and results type list.
    if (parser.parseAssignmentList(regionArgs, operands) ||
        parser.parseArrowTypeList(result.types))
      return failure();
    // Resolve input operands.
    for (auto argOperandType :
         llvm::zip(llvm::drop_begin(regionArgs), operands, result.types)) {
      Type type = std::get<2>(argOperandType);
      std::get<0>(argOperandType).type = type;
      if (parser.resolveOperand(std::get<1>(argOperandType), type,
                                result.operands))
        return failure();
    }
  }

  // Parse the body region.
  Region *body = result.addRegion();
  if (regionArgs.size() != result.types.size() + 1)
    return parser.emitError(
        parser.getNameLoc(),
        "mismatch between the number of loop-carried values and results");
  if (parser.parseRegion(*body, regionArgs))
    return failure();

  AffineForOp::ensureTerminator(*body, builder, result.location);

  // Parse the optional attribute list.
  return parser.parseOptionalAttrDict(result.attributes);
}

static void printBound(AffineMapAttr boundMap,
                       Operation::operand_range boundOperands,
                       const char *prefix, OpAsmPrinter &p) {
  AffineMap map = boundMap.getValue();

  // Check if this bound should be printed using custom assembly form.
  // The decision to restrict printing custom assembly form to trivial cases
  // comes from the will to roundtrip MLIR binary -> text -> binary in a
  // lossless way.
  // Therefore, custom assembly form parsing and printing is only supported for
  // zero-operand constant maps and single symbol operand identity maps.
  if (map.getNumResults() == 1) {
    AffineExpr expr = map.getResult(0);

    // Print constant bound.
    if (map.getNumDims() == 0 && map.getNumSymbols() == 0) {
      if (auto constExpr = expr.dyn_cast<AffineConstantExpr>()) {
        p << constExpr.getValue();
        return;
      }
    }

    // Print bound that consists of a single SSA symbol if the map is over a
    // single symbol.
    if (map.getNumDims() == 0 && map.getNumSymbols() == 1) {
      if (auto symExpr = expr.dyn_cast<AffineSymbolExpr>()) {
        p.printOperand(*boundOperands.begin());
        return;
      }
    }
  } else {
    // Map has multiple results. Print 'min' or 'max' prefix.
    p << prefix << ' ';
  }

  // Print the map and its operands.
  p << boundMap;
  printDimAndSymbolList(boundOperands.begin(), boundOperands.end(),
                        map.getNumDims(), p);
}

unsigned AffineForOp::getNumIterOperands() {
  AffineMap lbMap = getLowerBoundMapAttr().getValue();
  AffineMap ubMap = getUpperBoundMapAttr().getValue();

  return getNumOperands() - lbMap.getNumInputs() - ubMap.getNumInputs();
}

void AffineForOp::print(OpAsmPrinter &p) {
  p << ' ';
  p.printRegionArgument(getBody()->getArgument(0), /*argAttrs=*/{},
                        /*omitType=*/true);
  p << " = ";
  printBound(getLowerBoundMapAttr(), getLowerBoundOperands(), "max", p);
  p << " to ";
  printBound(getUpperBoundMapAttr(), getUpperBoundOperands(), "min", p);

  if (getStep() != 1)
    p << " step " << getStep();

  bool printBlockTerminators = false;
  if (getNumIterOperands() > 0) {
    p << " iter_args(";
    auto regionArgs = getRegionIterArgs();
    auto operands = getIterOperands();

    llvm::interleaveComma(llvm::zip(regionArgs, operands), p, [&](auto it) {
      p << std::get<0>(it) << " = " << std::get<1>(it);
    });
    p << ") -> (" << getResultTypes() << ")";
    printBlockTerminators = true;
  }

  p << ' ';
  p.printRegion(getRegion(), /*printEntryBlockArgs=*/false,
                printBlockTerminators);
  p.printOptionalAttrDict((*this)->getAttrs(),
                          /*elidedAttrs=*/{getLowerBoundAttrStrName(),
                                           getUpperBoundAttrStrName(),
                                           getStepAttrStrName()});
}

/// Fold the constant bounds of a loop.
static LogicalResult foldLoopBounds(AffineForOp forOp) {
  auto foldLowerOrUpperBound = [&forOp](bool lower) {
    // Check to see if each of the operands is the result of a constant.  If
    // so, get the value.  If not, ignore it.
    SmallVector<Attribute, 8> operandConstants;
    auto boundOperands =
        lower ? forOp.getLowerBoundOperands() : forOp.getUpperBoundOperands();
    for (auto operand : boundOperands) {
      Attribute operandCst;
      matchPattern(operand, m_Constant(&operandCst));
      operandConstants.push_back(operandCst);
    }

    AffineMap boundMap =
        lower ? forOp.getLowerBoundMap() : forOp.getUpperBoundMap();
    assert(boundMap.getNumResults() >= 1 &&
           "bound maps should have at least one result");
    SmallVector<Attribute, 4> foldedResults;
    if (failed(boundMap.constantFold(operandConstants, foldedResults)))
      return failure();

    // Compute the max or min as applicable over the results.
    assert(!foldedResults.empty() && "bounds should have at least one result");
    auto maxOrMin = llvm::cast<IntegerAttr>(foldedResults[0]).getValue();
    for (unsigned i = 1, e = foldedResults.size(); i < e; i++) {
      auto foldedResult = llvm::cast<IntegerAttr>(foldedResults[i]).getValue();
      maxOrMin = lower ? llvm::APIntOps::smax(maxOrMin, foldedResult)
                       : llvm::APIntOps::smin(maxOrMin, foldedResult);
    }
    lower ? forOp.setConstantLowerBound(maxOrMin.getSExtValue())
          : forOp.setConstantUpperBound(maxOrMin.getSExtValue());
    return success();
  };

  // Try to fold the lower bound.
  bool folded = false;
  if (!forOp.hasConstantLowerBound())
    folded |= succeeded(foldLowerOrUpperBound(/*lower=*/true));

  // Try to fold the upper bound.
  if (!forOp.hasConstantUpperBound())
    folded |= succeeded(foldLowerOrUpperBound(/*lower=*/false));
  return success(folded);
}

/// Canonicalize the bounds of the given loop.
static LogicalResult canonicalizeLoopBounds(AffineForOp forOp) {
  SmallVector<Value, 4> lbOperands(forOp.getLowerBoundOperands());
  SmallVector<Value, 4> ubOperands(forOp.getUpperBoundOperands());

  auto lbMap = forOp.getLowerBoundMap();
  auto ubMap = forOp.getUpperBoundMap();
  auto prevLbMap = lbMap;
  auto prevUbMap = ubMap;

  composeAffineMapAndOperands(&lbMap, &lbOperands);
  canonicalizeMapAndOperands(&lbMap, &lbOperands);
  simplifyMinOrMaxExprWithOperands(lbMap, lbOperands, /*isMax=*/true);
  simplifyMinOrMaxExprWithOperands(ubMap, ubOperands, /*isMax=*/false);
  lbMap = removeDuplicateExprs(lbMap);

  composeAffineMapAndOperands(&ubMap, &ubOperands);
  canonicalizeMapAndOperands(&ubMap, &ubOperands);
  ubMap = removeDuplicateExprs(ubMap);

  // Any canonicalization change always leads to updated map(s).
  if (lbMap == prevLbMap && ubMap == prevUbMap)
    return failure();

  if (lbMap != prevLbMap)
    forOp.setLowerBound(lbOperands, lbMap);
  if (ubMap != prevUbMap)
    forOp.setUpperBound(ubOperands, ubMap);
  return success();
}

namespace {
/// Returns constant trip count in trivial cases.
static std::optional<uint64_t> getTrivialConstantTripCount(AffineForOp forOp) {
  int64_t step = forOp.getStep();
  if (!forOp.hasConstantBounds() || step <= 0)
    return std::nullopt;
  int64_t lb = forOp.getConstantLowerBound();
  int64_t ub = forOp.getConstantUpperBound();
  return ub - lb <= 0 ? 0 : (ub - lb + step - 1) / step;
}

/// This is a pattern to fold trivially empty loop bodies.
/// TODO: This should be moved into the folding hook.
struct AffineForEmptyLoopFolder : public OpRewritePattern<AffineForOp> {
  using OpRewritePattern<AffineForOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(AffineForOp forOp,
                                PatternRewriter &rewriter) const override {
    // Check that the body only contains a yield.
    if (!llvm::hasSingleElement(*forOp.getBody()))
      return failure();
    if (forOp.getNumResults() == 0)
      return success();
    std::optional<uint64_t> tripCount = getTrivialConstantTripCount(forOp);
    if (tripCount && *tripCount == 0) {
      // The initial values of the iteration arguments would be the op's
      // results.
      rewriter.replaceOp(forOp, forOp.getIterOperands());
      return success();
    }
    SmallVector<Value, 4> replacements;
    auto yieldOp = cast<AffineYieldOp>(forOp.getBody()->getTerminator());
    auto iterArgs = forOp.getRegionIterArgs();
    bool hasValDefinedOutsideLoop = false;
    bool iterArgsNotInOrder = false;
    for (unsigned i = 0, e = yieldOp->getNumOperands(); i < e; ++i) {
      Value val = yieldOp.getOperand(i);
      auto *iterArgIt = llvm::find(iterArgs, val);
      if (iterArgIt == iterArgs.end()) {
        // `val` is defined outside of the loop.
        assert(forOp.isDefinedOutsideOfLoop(val) &&
               "must be defined outside of the loop");
        hasValDefinedOutsideLoop = true;
        replacements.push_back(val);
      } else {
        unsigned pos = std::distance(iterArgs.begin(), iterArgIt);
        if (pos != i)
          iterArgsNotInOrder = true;
        replacements.push_back(forOp.getIterOperands()[pos]);
      }
    }
    // Bail out when the trip count is unknown and the loop returns any value
    // defined outside of the loop or any iterArg out of order.
    if (!tripCount.has_value() &&
        (hasValDefinedOutsideLoop || iterArgsNotInOrder))
      return failure();
    // Bail out when the loop iterates more than once and it returns any iterArg
    // out of order.
    if (tripCount.has_value() && tripCount.value() >= 2 && iterArgsNotInOrder)
      return failure();
    rewriter.replaceOp(forOp, replacements);
    return success();
  }
};
} // namespace

void AffineForOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                              MLIRContext *context) {
  results.add<AffineForEmptyLoopFolder>(context);
}

/// Return operands used when entering the region at 'index'. These operands
/// correspond to the loop iterator operands, i.e., those excluding the
/// induction variable. AffineForOp only has one region, so zero is the only
/// valid value for `index`.
OperandRange
AffineForOp::getSuccessorEntryOperands(std::optional<unsigned> index) {
  assert((!index || *index == 0) && "invalid region index");

  // The initial operands map to the loop arguments after the induction
  // variable or are forwarded to the results when the trip count is zero.
  return getIterOperands();
}

/// Given the region at `index`, or the parent operation if `index` is None,
/// return the successor regions. These are the regions that may be selected
/// during the flow of control. `operands` is a set of optional attributes that
/// correspond to a constant value for each operand, or null if that operand is
/// not a constant.
void AffineForOp::getSuccessorRegions(
    std::optional<unsigned> index, ArrayRef<Attribute> operands,
    SmallVectorImpl<RegionSuccessor> &regions) {
  assert((!index.has_value() || index.value() == 0) && "expected loop region");
  // The loop may typically branch back to its body or to the parent operation.
  // If the predecessor is the parent op and the trip count is known to be at
  // least one, branch into the body using the iterator arguments. And in cases
  // we know the trip count is zero, it can only branch back to its parent.
  std::optional<uint64_t> tripCount = getTrivialConstantTripCount(*this);
  if (!index.has_value() && tripCount.has_value()) {
    if (tripCount.value() > 0) {
      regions.push_back(RegionSuccessor(&getLoopBody(), getRegionIterArgs()));
      return;
    }
    if (tripCount.value() == 0) {
      regions.push_back(RegionSuccessor(getResults()));
      return;
    }
  }

  // From the loop body, if the trip count is one, we can only branch back to
  // the parent.
  if (index && tripCount && *tripCount == 1) {
    regions.push_back(RegionSuccessor(getResults()));
    return;
  }

  // In all other cases, the loop may branch back to itself or the parent
  // operation.
  regions.push_back(RegionSuccessor(&getLoopBody(), getRegionIterArgs()));
  regions.push_back(RegionSuccessor(getResults()));
}

/// Returns true if the affine.for has zero iterations in trivial cases.
static bool hasTrivialZeroTripCount(AffineForOp op) {
  std::optional<uint64_t> tripCount = getTrivialConstantTripCount(op);
  return tripCount && *tripCount == 0;
}

LogicalResult AffineForOp::fold(FoldAdaptor adaptor,
                                SmallVectorImpl<OpFoldResult> &results) {
  bool folded = succeeded(foldLoopBounds(*this));
  folded |= succeeded(canonicalizeLoopBounds(*this));
  if (hasTrivialZeroTripCount(*this)) {
    // The initial values of the loop-carried variables (iter_args) are the
    // results of the op.
    results.assign(getIterOperands().begin(), getIterOperands().end());
    folded = true;
  }
  return success(folded);
}

AffineBound AffineForOp::getLowerBound() {
  auto lbMap = getLowerBoundMap();
  return AffineBound(AffineForOp(*this), 0, lbMap.getNumInputs(), lbMap);
}

AffineBound AffineForOp::getUpperBound() {
  auto lbMap = getLowerBoundMap();
  auto ubMap = getUpperBoundMap();
  return AffineBound(AffineForOp(*this), lbMap.getNumInputs(),
                     lbMap.getNumInputs() + ubMap.getNumInputs(), ubMap);
}

void AffineForOp::setLowerBound(ValueRange lbOperands, AffineMap map) {
  assert(lbOperands.size() == map.getNumInputs());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");

  SmallVector<Value, 4> newOperands(lbOperands.begin(), lbOperands.end());

  auto ubOperands = getUpperBoundOperands();
  newOperands.append(ubOperands.begin(), ubOperands.end());
  auto iterOperands = getIterOperands();
  newOperands.append(iterOperands.begin(), iterOperands.end());
  (*this)->setOperands(newOperands);

  (*this)->setAttr(getLowerBoundAttrStrName(), AffineMapAttr::get(map));
}

void AffineForOp::setUpperBound(ValueRange ubOperands, AffineMap map) {
  assert(ubOperands.size() == map.getNumInputs());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");

  SmallVector<Value, 4> newOperands(getLowerBoundOperands());
  newOperands.append(ubOperands.begin(), ubOperands.end());
  auto iterOperands = getIterOperands();
  newOperands.append(iterOperands.begin(), iterOperands.end());
  (*this)->setOperands(newOperands);

  (*this)->setAttr(getUpperBoundAttrStrName(), AffineMapAttr::get(map));
}

void AffineForOp::setLowerBoundMap(AffineMap map) {
  auto lbMap = getLowerBoundMap();
  assert(lbMap.getNumDims() == map.getNumDims() &&
         lbMap.getNumSymbols() == map.getNumSymbols());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");
  (void)lbMap;
  (*this)->setAttr(getLowerBoundAttrStrName(), AffineMapAttr::get(map));
}

void AffineForOp::setUpperBoundMap(AffineMap map) {
  auto ubMap = getUpperBoundMap();
  assert(ubMap.getNumDims() == map.getNumDims() &&
         ubMap.getNumSymbols() == map.getNumSymbols());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");
  (void)ubMap;
  (*this)->setAttr(getUpperBoundAttrStrName(), AffineMapAttr::get(map));
}

bool AffineForOp::hasConstantLowerBound() {
  return getLowerBoundMap().isSingleConstant();
}

bool AffineForOp::hasConstantUpperBound() {
  return getUpperBoundMap().isSingleConstant();
}

int64_t AffineForOp::getConstantLowerBound() {
  return getLowerBoundMap().getSingleConstantResult();
}

int64_t AffineForOp::getConstantUpperBound() {
  return getUpperBoundMap().getSingleConstantResult();
}

void AffineForOp::setConstantLowerBound(int64_t value) {
  setLowerBound({}, AffineMap::getConstantMap(value, getContext()));
}

void AffineForOp::setConstantUpperBound(int64_t value) {
  setUpperBound({}, AffineMap::getConstantMap(value, getContext()));
}

AffineForOp::operand_range AffineForOp::getLowerBoundOperands() {
  return {operand_begin(), operand_begin() + getLowerBoundMap().getNumInputs()};
}

AffineForOp::operand_range AffineForOp::getUpperBoundOperands() {
  return {operand_begin() + getLowerBoundMap().getNumInputs(),
          operand_begin() + getLowerBoundMap().getNumInputs() +
              getUpperBoundMap().getNumInputs()};
}

AffineForOp::operand_range AffineForOp::getControlOperands() {
  return {operand_begin(), operand_begin() + getLowerBoundMap().getNumInputs() +
                               getUpperBoundMap().getNumInputs()};
}

bool AffineForOp::matchingBoundOperandList() {
  auto lbMap = getLowerBoundMap();
  auto ubMap = getUpperBoundMap();
  if (lbMap.getNumDims() != ubMap.getNumDims() ||
      lbMap.getNumSymbols() != ubMap.getNumSymbols())
    return false;

  unsigned numOperands = lbMap.getNumInputs();
  for (unsigned i = 0, e = lbMap.getNumInputs(); i < e; i++) {
    // Compare Value 's.
    if (getOperand(i) != getOperand(numOperands + i))
      return false;
  }
  return true;
}

Region &AffineForOp::getLoopBody() { return getRegion(); }

std::optional<Value> AffineForOp::getSingleInductionVar() {
  return getInductionVar();
}

std::optional<OpFoldResult> AffineForOp::getSingleLowerBound() {
  if (!hasConstantLowerBound())
    return std::nullopt;
  OpBuilder b(getContext());
  return OpFoldResult(b.getI64IntegerAttr(getConstantLowerBound()));
}

std::optional<OpFoldResult> AffineForOp::getSingleStep() {
  OpBuilder b(getContext());
  return OpFoldResult(b.getI64IntegerAttr(getStep()));
}

std::optional<OpFoldResult> AffineForOp::getSingleUpperBound() {
  if (!hasConstantUpperBound())
    return std::nullopt;
  OpBuilder b(getContext());
  return OpFoldResult(b.getI64IntegerAttr(getConstantUpperBound()));
}

Speculation::Speculatability AffineForOp::getSpeculatability() {
  // `affine.for (I = Start; I < End; I += 1)` terminates for all values of
  // Start and End.
  //
  // For Step != 1, the loop may not terminate.  We can add more smarts here if
  // needed.
  return getStep() == 1 ? Speculation::RecursivelySpeculatable
                        : Speculation::NotSpeculatable;
}

/// Returns true if the provided value is the induction variable of a
/// AffineForOp.
bool mlir::affine::isAffineForInductionVar(Value val) {
  return getForInductionVarOwner(val) != AffineForOp();
}

bool mlir::affine::isAffineParallelInductionVar(Value val) {
  return getAffineParallelInductionVarOwner(val) != nullptr;
}

bool mlir::affine::isAffineInductionVar(Value val) {
  return isAffineForInductionVar(val) || isAffineParallelInductionVar(val);
}

AffineForOp mlir::affine::getForInductionVarOwner(Value val) {
  auto ivArg = llvm::dyn_cast<BlockArgument>(val);
  if (!ivArg || !ivArg.getOwner())
    return AffineForOp();
  auto *containingInst = ivArg.getOwner()->getParent()->getParentOp();
  if (auto forOp = dyn_cast<AffineForOp>(containingInst))
    // Check to make sure `val` is the induction variable, not an iter_arg.
    return forOp.getInductionVar() == val ? forOp : AffineForOp();
  return AffineForOp();
}

AffineParallelOp mlir::affine::getAffineParallelInductionVarOwner(Value val) {
  auto ivArg = llvm::dyn_cast<BlockArgument>(val);
  if (!ivArg || !ivArg.getOwner())
    return nullptr;
  Operation *containingOp = ivArg.getOwner()->getParentOp();
  auto parallelOp = dyn_cast<AffineParallelOp>(containingOp);
  if (parallelOp && llvm::is_contained(parallelOp.getIVs(), val))
    return parallelOp;
  return nullptr;
}

/// Extracts the induction variables from a list of AffineForOps and returns
/// them.
void mlir::affine::extractForInductionVars(ArrayRef<AffineForOp> forInsts,
                                           SmallVectorImpl<Value> *ivs) {
  ivs->reserve(forInsts.size());
  for (auto forInst : forInsts)
    ivs->push_back(forInst.getInductionVar());
}

void mlir::affine::extractInductionVars(ArrayRef<mlir::Operation *> affineOps,
                                        SmallVectorImpl<mlir::Value> &ivs) {
  ivs.reserve(affineOps.size());
  for (Operation *op : affineOps) {
    // Add constraints from forOp's bounds.
    if (auto forOp = dyn_cast<AffineForOp>(op))
      ivs.push_back(forOp.getInductionVar());
    else if (auto parallelOp = dyn_cast<AffineParallelOp>(op))
      for (size_t i = 0; i < parallelOp.getBody()->getNumArguments(); i++)
        ivs.push_back(parallelOp.getBody()->getArgument(i));
  }
}

/// Builds an affine loop nest, using "loopCreatorFn" to create individual loop
/// operations.
template <typename BoundListTy, typename LoopCreatorTy>
static void buildAffineLoopNestImpl(
    OpBuilder &builder, Location loc, BoundListTy lbs, BoundListTy ubs,
    ArrayRef<int64_t> steps,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuilderFn,
    LoopCreatorTy &&loopCreatorFn) {
  assert(lbs.size() == ubs.size() && "Mismatch in number of arguments");
  assert(lbs.size() == steps.size() && "Mismatch in number of arguments");

  // If there are no loops to be constructed, construct the body anyway.
  OpBuilder::InsertionGuard guard(builder);
  if (lbs.empty()) {
    if (bodyBuilderFn)
      bodyBuilderFn(builder, loc, ValueRange());
    return;
  }

  // Create the loops iteratively and store the induction variables.
  SmallVector<Value, 4> ivs;
  ivs.reserve(lbs.size());
  for (unsigned i = 0, e = lbs.size(); i < e; ++i) {
    // Callback for creating the loop body, always creates the terminator.
    auto loopBody = [&](OpBuilder &nestedBuilder, Location nestedLoc, Value iv,
                        ValueRange iterArgs) {
      ivs.push_back(iv);
      // In the innermost loop, call the body builder.
      if (i == e - 1 && bodyBuilderFn) {
        OpBuilder::InsertionGuard nestedGuard(nestedBuilder);
        bodyBuilderFn(nestedBuilder, nestedLoc, ivs);
      }
      nestedBuilder.create<AffineYieldOp>(nestedLoc);
    };

    // Delegate actual loop creation to the callback in order to dispatch
    // between constant- and variable-bound loops.
    auto loop = loopCreatorFn(builder, loc, lbs[i], ubs[i], steps[i], loopBody);
    builder.setInsertionPointToStart(loop.getBody());
  }
}

/// Creates an affine loop from the bounds known to be constants.
static AffineForOp
buildAffineLoopFromConstants(OpBuilder &builder, Location loc, int64_t lb,
                             int64_t ub, int64_t step,
                             AffineForOp::BodyBuilderFn bodyBuilderFn) {
  return builder.create<AffineForOp>(loc, lb, ub, step,
                                     /*iterArgs=*/std::nullopt, bodyBuilderFn);
}

/// Creates an affine loop from the bounds that may or may not be constants.
static AffineForOp
buildAffineLoopFromValues(OpBuilder &builder, Location loc, Value lb, Value ub,
                          int64_t step,
                          AffineForOp::BodyBuilderFn bodyBuilderFn) {
  auto lbConst = lb.getDefiningOp<arith::ConstantIndexOp>();
  auto ubConst = ub.getDefiningOp<arith::ConstantIndexOp>();
  if (lbConst && ubConst)
    return buildAffineLoopFromConstants(builder, loc, lbConst.value(),
                                        ubConst.value(), step, bodyBuilderFn);
  return builder.create<AffineForOp>(loc, lb, builder.getDimIdentityMap(), ub,
                                     builder.getDimIdentityMap(), step,
                                     /*iterArgs=*/std::nullopt, bodyBuilderFn);
}

void mlir::affine::buildAffineLoopNest(
    OpBuilder &builder, Location loc, ArrayRef<int64_t> lbs,
    ArrayRef<int64_t> ubs, ArrayRef<int64_t> steps,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuilderFn) {
  buildAffineLoopNestImpl(builder, loc, lbs, ubs, steps, bodyBuilderFn,
                          buildAffineLoopFromConstants);
}

void mlir::affine::buildAffineLoopNest(
    OpBuilder &builder, Location loc, ValueRange lbs, ValueRange ubs,
    ArrayRef<int64_t> steps,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuilderFn) {
  buildAffineLoopNestImpl(builder, loc, lbs, ubs, steps, bodyBuilderFn,
                          buildAffineLoopFromValues);
}

AffineForOp mlir::affine::replaceForOpWithNewYields(OpBuilder &b,
                                                    AffineForOp loop,
                                                    ValueRange newIterOperands,
                                                    ValueRange newYieldedValues,
                                                    ValueRange newIterArgs,
                                                    bool replaceLoopResults) {
  assert(newIterOperands.size() == newYieldedValues.size() &&
         "newIterOperands must be of the same size as newYieldedValues");
  // Create a new loop before the existing one, with the extra operands.
  OpBuilder::InsertionGuard g(b);
  b.setInsertionPoint(loop);
  auto operands = llvm::to_vector<4>(loop.getIterOperands());
  operands.append(newIterOperands.begin(), newIterOperands.end());
  SmallVector<Value, 4> lbOperands(loop.getLowerBoundOperands());
  SmallVector<Value, 4> ubOperands(loop.getUpperBoundOperands());
  SmallVector<Value, 4> steps(loop.getStep());
  auto lbMap = loop.getLowerBoundMap();
  auto ubMap = loop.getUpperBoundMap();
  AffineForOp newLoop =
      b.create<AffineForOp>(loop.getLoc(), lbOperands, lbMap, ubOperands, ubMap,
                            loop.getStep(), operands);
  // Take the body of the original parent loop.
  newLoop.getLoopBody().takeBody(loop.getLoopBody());
  for (Value val : newIterArgs)
    newLoop.getLoopBody().addArgument(val.getType(), val.getLoc());

  // Update yield operation with new values to be added.
  if (!newYieldedValues.empty()) {
    auto yield = cast<AffineYieldOp>(newLoop.getBody()->getTerminator());
    b.setInsertionPoint(yield);
    auto yieldOperands = llvm::to_vector<4>(yield.getOperands());
    yieldOperands.append(newYieldedValues.begin(), newYieldedValues.end());
    b.create<AffineYieldOp>(yield.getLoc(), yieldOperands);
    yield.erase();
  }
  if (replaceLoopResults) {
    for (auto it : llvm::zip(loop.getResults(), newLoop.getResults().take_front(
                                                    loop.getNumResults()))) {
      std::get<0>(it).replaceAllUsesWith(std::get<1>(it));
    }
  }
  return newLoop;
}

//===----------------------------------------------------------------------===//
// AffineIfOp
//===----------------------------------------------------------------------===//

namespace {
/// Remove else blocks that have nothing other than a zero value yield.
struct SimplifyDeadElse : public OpRewritePattern<AffineIfOp> {
  using OpRewritePattern<AffineIfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(AffineIfOp ifOp,
                                PatternRewriter &rewriter) const override {
    if (ifOp.getElseRegion().empty() ||
        !llvm::hasSingleElement(*ifOp.getElseBlock()) || ifOp.getNumResults())
      return failure();

    rewriter.startRootUpdate(ifOp);
    rewriter.eraseBlock(ifOp.getElseBlock());
    rewriter.finalizeRootUpdate(ifOp);
    return success();
  }
};

/// Removes affine.if cond if the condition is always true or false in certain
/// trivial cases. Promotes the then/else block in the parent operation block.
struct AlwaysTrueOrFalseIf : public OpRewritePattern<AffineIfOp> {
  using OpRewritePattern<AffineIfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(AffineIfOp op,
                                PatternRewriter &rewriter) const override {

    auto isTriviallyFalse = [](IntegerSet iSet) {
      return iSet.isEmptyIntegerSet();
    };

    auto isTriviallyTrue = [](IntegerSet iSet) {
      return (iSet.getNumEqualities() == 1 && iSet.getNumInequalities() == 0 &&
              iSet.getConstraint(0) == 0);
    };

    IntegerSet affineIfConditions = op.getIntegerSet();
    Block *blockToMove;
    if (isTriviallyFalse(affineIfConditions)) {
      // The absence, or equivalently, the emptiness of the else region need not
      // be checked when affine.if is returning results because if an affine.if
      // operation is returning results, it always has a non-empty else region.
      if (op.getNumResults() == 0 && !op.hasElse()) {
        // If the else region is absent, or equivalently, empty, remove the
        // affine.if operation (which is not returning any results).
        rewriter.eraseOp(op);
        return success();
      }
      blockToMove = op.getElseBlock();
    } else if (isTriviallyTrue(affineIfConditions)) {
      blockToMove = op.getThenBlock();
    } else {
      return failure();
    }
    Operation *blockToMoveTerminator = blockToMove->getTerminator();
    // Promote the "blockToMove" block to the parent operation block between the
    // prologue and epilogue of "op".
    rewriter.inlineBlockBefore(blockToMove, op);
    // Replace the "op" operation with the operands of the
    // "blockToMoveTerminator" operation. Note that "blockToMoveTerminator" is
    // the affine.yield operation present in the "blockToMove" block. It has no
    // operands when affine.if is not returning results and therefore, in that
    // case, replaceOp just erases "op". When affine.if is not returning
    // results, the affine.yield operation can be omitted. It gets inserted
    // implicitly.
    rewriter.replaceOp(op, blockToMoveTerminator->getOperands());
    // Erase the "blockToMoveTerminator" operation since it is now in the parent
    // operation block, which already has its own terminator.
    rewriter.eraseOp(blockToMoveTerminator);
    return success();
  }
};
} // namespace

/// AffineIfOp has two regions -- `then` and `else`. The flow of data should be
/// as follows: AffineIfOp -> `then`/`else` -> AffineIfOp
void AffineIfOp::getSuccessorRegions(
    std::optional<unsigned> index, ArrayRef<Attribute> operands,
    SmallVectorImpl<RegionSuccessor> &regions) {
  // If the predecessor is an AffineIfOp, then branching into both `then` and
  // `else` region is valid.
  if (!index.has_value()) {
    regions.reserve(2);
    regions.push_back(
        RegionSuccessor(&getThenRegion(), getThenRegion().getArguments()));
    // Don't consider the else region if it is empty.
    if (!getElseRegion().empty())
      regions.push_back(
          RegionSuccessor(&getElseRegion(), getElseRegion().getArguments()));
    return;
  }

  // If the predecessor is the `else`/`then` region, then branching into parent
  // op is valid.
  regions.push_back(RegionSuccessor(getResults()));
}

LogicalResult AffineIfOp::verify() {
  // Verify that we have a condition attribute.
  // FIXME: This should be specified in the arguments list in ODS.
  auto conditionAttr =
      (*this)->getAttrOfType<IntegerSetAttr>(getConditionAttrStrName());
  if (!conditionAttr)
    return emitOpError("requires an integer set attribute named 'condition'");

  // Verify that there are enough operands for the condition.
  IntegerSet condition = conditionAttr.getValue();
  if (getNumOperands() != condition.getNumInputs())
    return emitOpError("operand count and condition integer set dimension and "
                       "symbol count must match");

  // Verify that the operands are valid dimension/symbols.
  if (failed(verifyDimAndSymbolIdentifiers(*this, getOperands(),
                                           condition.getNumDims())))
    return failure();

  return success();
}

ParseResult AffineIfOp::parse(OpAsmParser &parser, OperationState &result) {
  // Parse the condition attribute set.
  IntegerSetAttr conditionAttr;
  unsigned numDims;
  if (parser.parseAttribute(conditionAttr,
                            AffineIfOp::getConditionAttrStrName(),
                            result.attributes) ||
      parseDimAndSymbolList(parser, result.operands, numDims))
    return failure();

  // Verify the condition operands.
  auto set = conditionAttr.getValue();
  if (set.getNumDims() != numDims)
    return parser.emitError(
        parser.getNameLoc(),
        "dim operand count and integer set dim count must match");
  if (numDims + set.getNumSymbols() != result.operands.size())
    return parser.emitError(
        parser.getNameLoc(),
        "symbol operand count and integer set symbol count must match");

  if (parser.parseOptionalArrowTypeList(result.types))
    return failure();

  // Create the regions for 'then' and 'else'.  The latter must be created even
  // if it remains empty for the validity of the operation.
  result.regions.reserve(2);
  Region *thenRegion = result.addRegion();
  Region *elseRegion = result.addRegion();

  // Parse the 'then' region.
  if (parser.parseRegion(*thenRegion, {}, {}))
    return failure();
  AffineIfOp::ensureTerminator(*thenRegion, parser.getBuilder(),
                               result.location);

  // If we find an 'else' keyword then parse the 'else' region.
  if (!parser.parseOptionalKeyword("else")) {
    if (parser.parseRegion(*elseRegion, {}, {}))
      return failure();
    AffineIfOp::ensureTerminator(*elseRegion, parser.getBuilder(),
                                 result.location);
  }

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();

  return success();
}

void AffineIfOp::print(OpAsmPrinter &p) {
  auto conditionAttr =
      (*this)->getAttrOfType<IntegerSetAttr>(getConditionAttrStrName());
  p << " " << conditionAttr;
  printDimAndSymbolList(operand_begin(), operand_end(),
                        conditionAttr.getValue().getNumDims(), p);
  p.printOptionalArrowTypeList(getResultTypes());
  p << ' ';
  p.printRegion(getThenRegion(), /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/getNumResults());

  // Print the 'else' regions if it has any blocks.
  auto &elseRegion = this->getElseRegion();
  if (!elseRegion.empty()) {
    p << " else ";
    p.printRegion(elseRegion,
                  /*printEntryBlockArgs=*/false,
                  /*printBlockTerminators=*/getNumResults());
  }

  // Print the attribute list.
  p.printOptionalAttrDict((*this)->getAttrs(),
                          /*elidedAttrs=*/getConditionAttrStrName());
}

IntegerSet AffineIfOp::getIntegerSet() {
  return (*this)
      ->getAttrOfType<IntegerSetAttr>(getConditionAttrStrName())
      .getValue();
}

void AffineIfOp::setIntegerSet(IntegerSet newSet) {
  (*this)->setAttr(getConditionAttrStrName(), IntegerSetAttr::get(newSet));
}

void AffineIfOp::setConditional(IntegerSet set, ValueRange operands) {
  setIntegerSet(set);
  (*this)->setOperands(operands);
}

void AffineIfOp::build(OpBuilder &builder, OperationState &result,
                       TypeRange resultTypes, IntegerSet set, ValueRange args,
                       bool withElseRegion) {
  assert(resultTypes.empty() || withElseRegion);
  result.addTypes(resultTypes);
  result.addOperands(args);
  result.addAttribute(getConditionAttrStrName(), IntegerSetAttr::get(set));

  Region *thenRegion = result.addRegion();
  thenRegion->push_back(new Block());
  if (resultTypes.empty())
    AffineIfOp::ensureTerminator(*thenRegion, builder, result.location);

  Region *elseRegion = result.addRegion();
  if (withElseRegion) {
    elseRegion->push_back(new Block());
    if (resultTypes.empty())
      AffineIfOp::ensureTerminator(*elseRegion, builder, result.location);
  }
}

void AffineIfOp::build(OpBuilder &builder, OperationState &result,
                       IntegerSet set, ValueRange args, bool withElseRegion) {
  AffineIfOp::build(builder, result, /*resultTypes=*/{}, set, args,
                    withElseRegion);
}

/// Compose any affine.apply ops feeding into `operands` of the integer set
/// `set` by composing the maps of such affine.apply ops with the integer
/// set constraints.
static void composeSetAndOperands(IntegerSet &set,
                                  SmallVectorImpl<Value> &operands) {
  // We will simply reuse the API of the map composition by viewing the LHSs of
  // the equalities and inequalities of `set` as the affine exprs of an affine
  // map. Convert to equivalent map, compose, and convert back to set.
  auto map = AffineMap::get(set.getNumDims(), set.getNumSymbols(),
                            set.getConstraints(), set.getContext());
  // Check if any composition is possible.
  if (llvm::none_of(operands,
                    [](Value v) { return v.getDefiningOp<AffineApplyOp>(); }))
    return;

  composeAffineMapAndOperands(&map, &operands);
  set = IntegerSet::get(map.getNumDims(), map.getNumSymbols(), map.getResults(),
                        set.getEqFlags());
}

/// Canonicalize an affine if op's conditional (integer set + operands).
LogicalResult AffineIfOp::fold(FoldAdaptor,
                               SmallVectorImpl<OpFoldResult> &) {
  auto set = getIntegerSet();
  SmallVector<Value, 4> operands(getOperands());
  composeSetAndOperands(set, operands);
  canonicalizeSetAndOperands(&set, &operands);

  // Check if the canonicalization or composition led to any change.
  if (getIntegerSet() == set && llvm::equal(operands, getOperands()))
    return failure();

  setConditional(set, operands);
  return success();
}

void AffineIfOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                             MLIRContext *context) {
  results.add<SimplifyDeadElse, AlwaysTrueOrFalseIf>(context);
}

//===----------------------------------------------------------------------===//
// AffineLoadOp
//===----------------------------------------------------------------------===//

void AffineLoadOp::build(OpBuilder &builder, OperationState &result,
                         AffineMap map, ValueRange operands) {
  assert(operands.size() == 1 + map.getNumInputs() && "inconsistent operands");
  result.addOperands(operands);
  if (map)
    result.addAttribute(getMapAttrStrName(), AffineMapAttr::get(map));
  auto memrefType = llvm::cast<MemRefType>(operands[0].getType());
  result.types.push_back(memrefType.getElementType());
}

void AffineLoadOp::build(OpBuilder &builder, OperationState &result,
                         Value memref, AffineMap map, ValueRange mapOperands) {
  assert(map.getNumInputs() == mapOperands.size() && "inconsistent index info");
  result.addOperands(memref);
  result.addOperands(mapOperands);
  auto memrefType = llvm::cast<MemRefType>(memref.getType());
  result.addAttribute(getMapAttrStrName(), AffineMapAttr::get(map));
  result.types.push_back(memrefType.getElementType());
}

void AffineLoadOp::build(OpBuilder &builder, OperationState &result,
                         Value memref, ValueRange indices) {
  auto memrefType = llvm::cast<MemRefType>(memref.getType());
  int64_t rank = memrefType.getRank();
  // Create identity map for memrefs with at least one dimension or () -> ()
  // for zero-dimensional memrefs.
  auto map =
      rank ? builder.getMultiDimIdentityMap(rank) : builder.getEmptyAffineMap();
  build(builder, result, memref, map, indices);
}

ParseResult AffineLoadOp::parse(OpAsmParser &parser, OperationState &result) {
  auto &builder = parser.getBuilder();
  auto indexTy = builder.getIndexType();

  MemRefType type;
  OpAsmParser::UnresolvedOperand memrefInfo;
  AffineMapAttr mapAttr;
  SmallVector<OpAsmParser::UnresolvedOperand, 1> mapOperands;
  return failure(
      parser.parseOperand(memrefInfo) ||
      parser.parseAffineMapOfSSAIds(mapOperands, mapAttr,
                                    AffineLoadOp::getMapAttrStrName(),
                                    result.attributes) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(type) ||
      parser.resolveOperand(memrefInfo, type, result.operands) ||
      parser.resolveOperands(mapOperands, indexTy, result.operands) ||
      parser.addTypeToList(type.getElementType(), result.types));
}

void AffineLoadOp::print(OpAsmPrinter &p) {
  p << " " << getMemRef() << '[';
  if (AffineMapAttr mapAttr =
          (*this)->getAttrOfType<AffineMapAttr>(getMapAttrStrName()))
    p.printAffineMapOfSSAIds(mapAttr, getMapOperands());
  p << ']';
  p.printOptionalAttrDict((*this)->getAttrs(),
                          /*elidedAttrs=*/{getMapAttrStrName()});
  p << " : " << getMemRefType();
}

/// Verify common indexing invariants of affine.load, affine.store,
/// affine.vector_load and affine.vector_store.
static LogicalResult
verifyMemoryOpIndexing(Operation *op, AffineMapAttr mapAttr,
                       Operation::operand_range mapOperands,
                       MemRefType memrefType, unsigned numIndexOperands) {
  if (mapAttr) {
    AffineMap map = mapAttr.getValue();
    if (map.getNumResults() != memrefType.getRank())
      return op->emitOpError("affine map num results must equal memref rank");
    if (map.getNumInputs() != numIndexOperands)
      return op->emitOpError("expects as many subscripts as affine map inputs");
  } else {
    if (memrefType.getRank() != numIndexOperands)
      return op->emitOpError(
          "expects the number of subscripts to be equal to memref rank");
  }

  Region *scope = getAffineScope(op);
  for (auto idx : mapOperands) {
    if (!idx.getType().isIndex())
      return op->emitOpError("index to load must have 'index' type");
    if (!isValidAffineIndexOperand(idx, scope))
      return op->emitOpError("index must be a dimension or symbol identifier");
  }

  return success();
}

LogicalResult AffineLoadOp::verify() {
  auto memrefType = getMemRefType();
  if (getType() != memrefType.getElementType())
    return emitOpError("result type must match element type of memref");

  if (failed(verifyMemoryOpIndexing(
          getOperation(),
          (*this)->getAttrOfType<AffineMapAttr>(getMapAttrStrName()),
          getMapOperands(), memrefType,
          /*numIndexOperands=*/getNumOperands() - 1)))
    return failure();

  return success();
}

void AffineLoadOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                               MLIRContext *context) {
  results.add<SimplifyAffineOp<AffineLoadOp>>(context);
}

OpFoldResult AffineLoadOp::fold(FoldAdaptor adaptor) {
  /// load(memrefcast) -> load
  if (succeeded(memref::foldMemRefCast(*this)))
    return getResult();

  // Fold load from a global constant memref.
  auto getGlobalOp = getMemref().getDefiningOp<memref::GetGlobalOp>();
  if (!getGlobalOp)
    return {};
  // Get to the memref.global defining the symbol.
  auto *symbolTableOp = getGlobalOp->getParentWithTrait<OpTrait::SymbolTable>();
  if (!symbolTableOp)
    return {};
  auto global = dyn_cast_or_null<memref::GlobalOp>(
      SymbolTable::lookupSymbolIn(symbolTableOp, getGlobalOp.getNameAttr()));
  if (!global)
    return {};

  // Check if the global memref is a constant.
  auto cstAttr =
      llvm::dyn_cast_or_null<DenseElementsAttr>(global.getConstantInitValue());
  if (!cstAttr)
    return {};
  // If it's a splat constant, we can fold irrespective of indices.
  if (auto splatAttr = llvm::dyn_cast<SplatElementsAttr>(cstAttr))
    return splatAttr.getSplatValue<Attribute>();
  // Otherwise, we can fold only if we know the indices.
  if (!getAffineMap().isConstant())
    return {};
  auto indices = llvm::to_vector<4>(
      llvm::map_range(getAffineMap().getConstantResults(),
                      [](int64_t v) -> uint64_t { return v; }));
  return cstAttr.getValues<Attribute>()[indices];
}

//===----------------------------------------------------------------------===//
// AffineStoreOp
//===----------------------------------------------------------------------===//

void AffineStoreOp::build(OpBuilder &builder, OperationState &result,
                          Value valueToStore, Value memref, AffineMap map,
                          ValueRange mapOperands) {
  assert(map.getNumInputs() == mapOperands.size() && "inconsistent index info");
  result.addOperands(valueToStore);
  result.addOperands(memref);
  result.addOperands(mapOperands);
  result.addAttribute(getMapAttrStrName(), AffineMapAttr::get(map));
}

// Use identity map.
void AffineStoreOp::build(OpBuilder &builder, OperationState &result,
                          Value valueToStore, Value memref,
                          ValueRange indices) {
  auto memrefType = llvm::cast<MemRefType>(memref.getType());
  int64_t rank = memrefType.getRank();
  // Create identity map for memrefs with at least one dimension or () -> ()
  // for zero-dimensional memrefs.
  auto map =
      rank ? builder.getMultiDimIdentityMap(rank) : builder.getEmptyAffineMap();
  build(builder, result, valueToStore, memref, map, indices);
}

ParseResult AffineStoreOp::parse(OpAsmParser &parser, OperationState &result) {
  auto indexTy = parser.getBuilder().getIndexType();

  MemRefType type;
  OpAsmParser::UnresolvedOperand storeValueInfo;
  OpAsmParser::UnresolvedOperand memrefInfo;
  AffineMapAttr mapAttr;
  SmallVector<OpAsmParser::UnresolvedOperand, 1> mapOperands;
  return failure(parser.parseOperand(storeValueInfo) || parser.parseComma() ||
                 parser.parseOperand(memrefInfo) ||
                 parser.parseAffineMapOfSSAIds(
                     mapOperands, mapAttr, AffineStoreOp::getMapAttrStrName(),
                     result.attributes) ||
                 parser.parseOptionalAttrDict(result.attributes) ||
                 parser.parseColonType(type) ||
                 parser.resolveOperand(storeValueInfo, type.getElementType(),
                                       result.operands) ||
                 parser.resolveOperand(memrefInfo, type, result.operands) ||
                 parser.resolveOperands(mapOperands, indexTy, result.operands));
}

void AffineStoreOp::print(OpAsmPrinter &p) {
  p << " " << getValueToStore();
  p << ", " << getMemRef() << '[';
  if (AffineMapAttr mapAttr =
          (*this)->getAttrOfType<AffineMapAttr>(getMapAttrStrName()))
    p.printAffineMapOfSSAIds(mapAttr, getMapOperands());
  p << ']';
  p.printOptionalAttrDict((*this)->getAttrs(),
                          /*elidedAttrs=*/{getMapAttrStrName()});
  p << " : " << getMemRefType();
}

LogicalResult AffineStoreOp::verify() {
  // The value to store must have the same type as memref element type.
  auto memrefType = getMemRefType();
  if (getValueToStore().getType() != memrefType.getElementType())
    return emitOpError(
        "value to store must have the same type as memref element type");

  if (failed(verifyMemoryOpIndexing(
          getOperation(),
          (*this)->getAttrOfType<AffineMapAttr>(getMapAttrStrName()),
          getMapOperands(), memrefType,
          /*numIndexOperands=*/getNumOperands() - 2)))
    return failure();

  return success();
}

void AffineStoreOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                MLIRContext *context) {
  results.add<SimplifyAffineOp<AffineStoreOp>>(context);
}

LogicalResult AffineStoreOp::fold(FoldAdaptor adaptor,
                                  SmallVectorImpl<OpFoldResult> &results) {
  /// store(memrefcast) -> store
  return memref::foldMemRefCast(*this, getValueToStore());
}

//===----------------------------------------------------------------------===//
// AffineMinMaxOpBase
//===----------------------------------------------------------------------===//

template <typename T>
static LogicalResult verifyAffineMinMaxOp(T op) {
  // Verify that operand count matches affine map dimension and symbol count.
  if (op.getNumOperands() !=
      op.getMap().getNumDims() + op.getMap().getNumSymbols())
    return op.emitOpError(
        "operand count and affine map dimension and symbol count must match");
  return success();
}

template <typename T>
static void printAffineMinMaxOp(OpAsmPrinter &p, T op) {
  p << ' ' << op->getAttr(T::getMapAttrStrName());
  auto operands = op.getOperands();
  unsigned numDims = op.getMap().getNumDims();
  p << '(' << operands.take_front(numDims) << ')';

  if (operands.size() != numDims)
    p << '[' << operands.drop_front(numDims) << ']';
  p.printOptionalAttrDict(op->getAttrs(),
                          /*elidedAttrs=*/{T::getMapAttrStrName()});
}

template <typename T>
static ParseResult parseAffineMinMaxOp(OpAsmParser &parser,
                                       OperationState &result) {
  auto &builder = parser.getBuilder();
  auto indexType = builder.getIndexType();
  SmallVector<OpAsmParser::UnresolvedOperand, 8> dimInfos;
  SmallVector<OpAsmParser::UnresolvedOperand, 8> symInfos;
  AffineMapAttr mapAttr;
  return failure(
      parser.parseAttribute(mapAttr, T::getMapAttrStrName(),
                            result.attributes) ||
      parser.parseOperandList(dimInfos, OpAsmParser::Delimiter::Paren) ||
      parser.parseOperandList(symInfos,
                              OpAsmParser::Delimiter::OptionalSquare) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.resolveOperands(dimInfos, indexType, result.operands) ||
      parser.resolveOperands(symInfos, indexType, result.operands) ||
      parser.addTypeToList(indexType, result.types));
}

/// Fold an affine min or max operation with the given operands. The operand
/// list may contain nulls, which are interpreted as the operand not being a
/// constant.
template <typename T>
static OpFoldResult foldMinMaxOp(T op, ArrayRef<Attribute> operands) {
  static_assert(llvm::is_one_of<T, AffineMinOp, AffineMaxOp>::value,
                "expected affine min or max op");

  // Fold the affine map.
  // TODO: Fold more cases:
  // min(some_affine, some_affine + constant, ...), etc.
  SmallVector<int64_t, 2> results;
  auto foldedMap = op.getMap().partialConstantFold(operands, &results);

  if (foldedMap.getNumSymbols() == 1 && foldedMap.isSymbolIdentity())
    return op.getOperand(0);

  // If some of the map results are not constant, try changing the map in-place.
  if (results.empty()) {
    // If the map is the same, report that folding did not happen.
    if (foldedMap == op.getMap())
      return {};
    op->setAttr("map", AffineMapAttr::get(foldedMap));
    return op.getResult();
  }

  // Otherwise, completely fold the op into a constant.
  auto resultIt = std::is_same<T, AffineMinOp>::value
                      ? std::min_element(results.begin(), results.end())
                      : std::max_element(results.begin(), results.end());
  if (resultIt == results.end())
    return {};
  return IntegerAttr::get(IndexType::get(op.getContext()), *resultIt);
}

/// Remove duplicated expressions in affine min/max ops.
template <typename T>
struct DeduplicateAffineMinMaxExpressions : public OpRewritePattern<T> {
  using OpRewritePattern<T>::OpRewritePattern;

  LogicalResult matchAndRewrite(T affineOp,
                                PatternRewriter &rewriter) const override {
    AffineMap oldMap = affineOp.getAffineMap();

    SmallVector<AffineExpr, 4> newExprs;
    for (AffineExpr expr : oldMap.getResults()) {
      // This is a linear scan over newExprs, but it should be fine given that
      // we typically just have a few expressions per op.
      if (!llvm::is_contained(newExprs, expr))
        newExprs.push_back(expr);
    }

    if (newExprs.size() == oldMap.getNumResults())
      return failure();

    auto newMap = AffineMap::get(oldMap.getNumDims(), oldMap.getNumSymbols(),
                                 newExprs, rewriter.getContext());
    rewriter.replaceOpWithNewOp<T>(affineOp, newMap, affineOp.getMapOperands());

    return success();
  }
};

/// Merge an affine min/max op to its consumers if its consumer is also an
/// affine min/max op.
///
/// This pattern requires the producer affine min/max op is bound to a
/// dimension/symbol that is used as a standalone expression in the consumer
/// affine op's map.
///
/// For example, a pattern like the following:
///
///   %0 = affine.min affine_map<()[s0] -> (s0 + 16, s0 * 8)> ()[%sym1]
///   %1 = affine.min affine_map<(d0)[s0] -> (s0 + 4, d0)> (%0)[%sym2]
///
/// Can be turned into:
///
///   %1 = affine.min affine_map<
///          ()[s0, s1] -> (s0 + 4, s1 + 16, s1 * 8)> ()[%sym2, %sym1]
template <typename T>
struct MergeAffineMinMaxOp : public OpRewritePattern<T> {
  using OpRewritePattern<T>::OpRewritePattern;

  LogicalResult matchAndRewrite(T affineOp,
                                PatternRewriter &rewriter) const override {
    AffineMap oldMap = affineOp.getAffineMap();
    ValueRange dimOperands =
        affineOp.getMapOperands().take_front(oldMap.getNumDims());
    ValueRange symOperands =
        affineOp.getMapOperands().take_back(oldMap.getNumSymbols());

    auto newDimOperands = llvm::to_vector<8>(dimOperands);
    auto newSymOperands = llvm::to_vector<8>(symOperands);
    SmallVector<AffineExpr, 4> newExprs;
    SmallVector<T, 4> producerOps;

    // Go over each expression to see whether it's a single dimension/symbol
    // with the corresponding operand which is the result of another affine
    // min/max op. If So it can be merged into this affine op.
    for (AffineExpr expr : oldMap.getResults()) {
      if (auto symExpr = expr.dyn_cast<AffineSymbolExpr>()) {
        Value symValue = symOperands[symExpr.getPosition()];
        if (auto producerOp = symValue.getDefiningOp<T>()) {
          producerOps.push_back(producerOp);
          continue;
        }
      } else if (auto dimExpr = expr.dyn_cast<AffineDimExpr>()) {
        Value dimValue = dimOperands[dimExpr.getPosition()];
        if (auto producerOp = dimValue.getDefiningOp<T>()) {
          producerOps.push_back(producerOp);
          continue;
        }
      }
      // For the above cases we will remove the expression by merging the
      // producer affine min/max's affine expressions. Otherwise we need to
      // keep the existing expression.
      newExprs.push_back(expr);
    }

    if (producerOps.empty())
      return failure();

    unsigned numUsedDims = oldMap.getNumDims();
    unsigned numUsedSyms = oldMap.getNumSymbols();

    // Now go over all producer affine ops and merge their expressions.
    for (T producerOp : producerOps) {
      AffineMap producerMap = producerOp.getAffineMap();
      unsigned numProducerDims = producerMap.getNumDims();
      unsigned numProducerSyms = producerMap.getNumSymbols();

      // Collect all dimension/symbol values.
      ValueRange dimValues =
          producerOp.getMapOperands().take_front(numProducerDims);
      ValueRange symValues =
          producerOp.getMapOperands().take_back(numProducerSyms);
      newDimOperands.append(dimValues.begin(), dimValues.end());
      newSymOperands.append(symValues.begin(), symValues.end());

      // For expressions we need to shift to avoid overlap.
      for (AffineExpr expr : producerMap.getResults()) {
        newExprs.push_back(expr.shiftDims(numProducerDims, numUsedDims)
                               .shiftSymbols(numProducerSyms, numUsedSyms));
      }

      numUsedDims += numProducerDims;
      numUsedSyms += numProducerSyms;
    }

    auto newMap = AffineMap::get(numUsedDims, numUsedSyms, newExprs,
                                 rewriter.getContext());
    auto newOperands =
        llvm::to_vector<8>(llvm::concat<Value>(newDimOperands, newSymOperands));
    rewriter.replaceOpWithNewOp<T>(affineOp, newMap, newOperands);

    return success();
  }
};

/// Canonicalize the result expression order of an affine map and return success
/// if the order changed.
///
/// The function flattens the map's affine expressions to coefficient arrays and
/// sorts them in lexicographic order. A coefficient array contains a multiplier
/// for every dimension/symbol and a constant term. The canonicalization fails
/// if a result expression is not pure or if the flattening requires local
/// variables that, unlike dimensions and symbols, have no global order.
static LogicalResult canonicalizeMapExprAndTermOrder(AffineMap &map) {
  SmallVector<SmallVector<int64_t>> flattenedExprs;
  for (const AffineExpr &resultExpr : map.getResults()) {
    // Fail if the expression is not pure.
    if (!resultExpr.isPureAffine())
      return failure();

    SimpleAffineExprFlattener flattener(map.getNumDims(), map.getNumSymbols());
    flattener.walkPostOrder(resultExpr);

    // Fail if the flattened expression has local variables.
    if (flattener.operandExprStack.back().size() !=
        map.getNumDims() + map.getNumSymbols() + 1)
      return failure();

    flattenedExprs.emplace_back(flattener.operandExprStack.back().begin(),
                                flattener.operandExprStack.back().end());
  }

  // Fail if sorting is not necessary.
  if (llvm::is_sorted(flattenedExprs))
    return failure();

  // Reorder the result expressions according to their flattened form.
  SmallVector<unsigned> resultPermutation =
      llvm::to_vector(llvm::seq<unsigned>(0, map.getNumResults()));
  llvm::sort(resultPermutation, [&](unsigned lhs, unsigned rhs) {
    return flattenedExprs[lhs] < flattenedExprs[rhs];
  });
  SmallVector<AffineExpr> newExprs;
  for (unsigned idx : resultPermutation)
    newExprs.push_back(map.getResult(idx));

  map = AffineMap::get(map.getNumDims(), map.getNumSymbols(), newExprs,
                       map.getContext());
  return success();
}

/// Canonicalize the affine map result expression order of an affine min/max
/// operation.
///
/// The pattern calls `canonicalizeMapExprAndTermOrder` to order the result
/// expressions and replaces the operation if the order changed.
///
/// For example, the following operation:
///
///   %0 = affine.min affine_map<(d0, d1) -> (d0 + d1, d1 + 16, 32)> (%i0, %i1)
///
/// Turns into:
///
///   %0 = affine.min affine_map<(d0, d1) -> (32, d1 + 16, d0 + d1)> (%i0, %i1)
template <typename T>
struct CanonicalizeAffineMinMaxOpExprAndTermOrder : public OpRewritePattern<T> {
  using OpRewritePattern<T>::OpRewritePattern;

  LogicalResult matchAndRewrite(T affineOp,
                                PatternRewriter &rewriter) const override {
    AffineMap map = affineOp.getAffineMap();
    if (failed(canonicalizeMapExprAndTermOrder(map)))
      return failure();
    rewriter.replaceOpWithNewOp<T>(affineOp, map, affineOp.getMapOperands());
    return success();
  }
};

template <typename T>
struct CanonicalizeSingleResultAffineMinMaxOp : public OpRewritePattern<T> {
  using OpRewritePattern<T>::OpRewritePattern;

  LogicalResult matchAndRewrite(T affineOp,
                                PatternRewriter &rewriter) const override {
    if (affineOp.getMap().getNumResults() != 1)
      return failure();
    rewriter.replaceOpWithNewOp<AffineApplyOp>(affineOp, affineOp.getMap(),
                                               affineOp.getOperands());
    return success();
  }
};

//===----------------------------------------------------------------------===//
// AffineMinOp
//===----------------------------------------------------------------------===//
//
//   %0 = affine.min (d0) -> (1000, d0 + 512) (%i0)
//

OpFoldResult AffineMinOp::fold(FoldAdaptor adaptor) {
  return foldMinMaxOp(*this, adaptor.getOperands());
}

void AffineMinOp::getCanonicalizationPatterns(RewritePatternSet &patterns,
                                              MLIRContext *context) {
  patterns.add<CanonicalizeSingleResultAffineMinMaxOp<AffineMinOp>,
               DeduplicateAffineMinMaxExpressions<AffineMinOp>,
               MergeAffineMinMaxOp<AffineMinOp>, SimplifyAffineOp<AffineMinOp>,
               CanonicalizeAffineMinMaxOpExprAndTermOrder<AffineMinOp>>(
      context);
}

LogicalResult AffineMinOp::verify() { return verifyAffineMinMaxOp(*this); }

ParseResult AffineMinOp::parse(OpAsmParser &parser, OperationState &result) {
  return parseAffineMinMaxOp<AffineMinOp>(parser, result);
}

void AffineMinOp::print(OpAsmPrinter &p) { printAffineMinMaxOp(p, *this); }

//===----------------------------------------------------------------------===//
// AffineMaxOp
//===----------------------------------------------------------------------===//
//
//   %0 = affine.max (d0) -> (1000, d0 + 512) (%i0)
//

OpFoldResult AffineMaxOp::fold(FoldAdaptor adaptor) {
  return foldMinMaxOp(*this, adaptor.getOperands());
}

void AffineMaxOp::getCanonicalizationPatterns(RewritePatternSet &patterns,
                                              MLIRContext *context) {
  patterns.add<CanonicalizeSingleResultAffineMinMaxOp<AffineMaxOp>,
               DeduplicateAffineMinMaxExpressions<AffineMaxOp>,
               MergeAffineMinMaxOp<AffineMaxOp>, SimplifyAffineOp<AffineMaxOp>,
               CanonicalizeAffineMinMaxOpExprAndTermOrder<AffineMaxOp>>(
      context);
}

LogicalResult AffineMaxOp::verify() { return verifyAffineMinMaxOp(*this); }

ParseResult AffineMaxOp::parse(OpAsmParser &parser, OperationState &result) {
  return parseAffineMinMaxOp<AffineMaxOp>(parser, result);
}

void AffineMaxOp::print(OpAsmPrinter &p) { printAffineMinMaxOp(p, *this); }

//===----------------------------------------------------------------------===//
// AffinePrefetchOp
//===----------------------------------------------------------------------===//

//
// affine.prefetch %0[%i, %j + 5], read, locality<3>, data : memref<400x400xi32>
//
ParseResult AffinePrefetchOp::parse(OpAsmParser &parser,
                                    OperationState &result) {
  auto &builder = parser.getBuilder();
  auto indexTy = builder.getIndexType();

  MemRefType type;
  OpAsmParser::UnresolvedOperand memrefInfo;
  IntegerAttr hintInfo;
  auto i32Type = parser.getBuilder().getIntegerType(32);
  StringRef readOrWrite, cacheType;

  AffineMapAttr mapAttr;
  SmallVector<OpAsmParser::UnresolvedOperand, 1> mapOperands;
  if (parser.parseOperand(memrefInfo) ||
      parser.parseAffineMapOfSSAIds(mapOperands, mapAttr,
                                    AffinePrefetchOp::getMapAttrStrName(),
                                    result.attributes) ||
      parser.parseComma() || parser.parseKeyword(&readOrWrite) ||
      parser.parseComma() || parser.parseKeyword("locality") ||
      parser.parseLess() ||
      parser.parseAttribute(hintInfo, i32Type,
                            AffinePrefetchOp::getLocalityHintAttrStrName(),
                            result.attributes) ||
      parser.parseGreater() || parser.parseComma() ||
      parser.parseKeyword(&cacheType) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(type) ||
      parser.resolveOperand(memrefInfo, type, result.operands) ||
      parser.resolveOperands(mapOperands, indexTy, result.operands))
    return failure();

  if (!readOrWrite.equals("read") && !readOrWrite.equals("write"))
    return parser.emitError(parser.getNameLoc(),
                            "rw specifier has to be 'read' or 'write'");
  result.addAttribute(
      AffinePrefetchOp::getIsWriteAttrStrName(),
      parser.getBuilder().getBoolAttr(readOrWrite.equals("write")));

  if (!cacheType.equals("data") && !cacheType.equals("instr"))
    return parser.emitError(parser.getNameLoc(),
                            "cache type has to be 'data' or 'instr'");

  result.addAttribute(
      AffinePrefetchOp::getIsDataCacheAttrStrName(),
      parser.getBuilder().getBoolAttr(cacheType.equals("data")));

  return success();
}

void AffinePrefetchOp::print(OpAsmPrinter &p) {
  p << " " << getMemref() << '[';
  AffineMapAttr mapAttr =
      (*this)->getAttrOfType<AffineMapAttr>(getMapAttrStrName());
  if (mapAttr)
    p.printAffineMapOfSSAIds(mapAttr, getMapOperands());
  p << ']' << ", " << (getIsWrite() ? "write" : "read") << ", "
    << "locality<" << getLocalityHint() << ">, "
    << (getIsDataCache() ? "data" : "instr");
  p.printOptionalAttrDict(
      (*this)->getAttrs(),
      /*elidedAttrs=*/{getMapAttrStrName(), getLocalityHintAttrStrName(),
                       getIsDataCacheAttrStrName(), getIsWriteAttrStrName()});
  p << " : " << getMemRefType();
}

LogicalResult AffinePrefetchOp::verify() {
  auto mapAttr = (*this)->getAttrOfType<AffineMapAttr>(getMapAttrStrName());
  if (mapAttr) {
    AffineMap map = mapAttr.getValue();
    if (map.getNumResults() != getMemRefType().getRank())
      return emitOpError("affine.prefetch affine map num results must equal"
                         " memref rank");
    if (map.getNumInputs() + 1 != getNumOperands())
      return emitOpError("too few operands");
  } else {
    if (getNumOperands() != 1)
      return emitOpError("too few operands");
  }

  Region *scope = getAffineScope(*this);
  for (auto idx : getMapOperands()) {
    if (!isValidAffineIndexOperand(idx, scope))
      return emitOpError("index must be a dimension or symbol identifier");
  }
  return success();
}

void AffinePrefetchOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                   MLIRContext *context) {
  // prefetch(memrefcast) -> prefetch
  results.add<SimplifyAffineOp<AffinePrefetchOp>>(context);
}

LogicalResult AffinePrefetchOp::fold(FoldAdaptor adaptor,
                                     SmallVectorImpl<OpFoldResult> &results) {
  /// prefetch(memrefcast) -> prefetch
  return memref::foldMemRefCast(*this);
}

//===----------------------------------------------------------------------===//
// AffineParallelOp
//===----------------------------------------------------------------------===//

void AffineParallelOp::build(OpBuilder &builder, OperationState &result,
                             TypeRange resultTypes,
                             ArrayRef<arith::AtomicRMWKind> reductions,
                             ArrayRef<int64_t> ranges) {
  SmallVector<AffineMap> lbs(ranges.size(), builder.getConstantAffineMap(0));
  auto ubs = llvm::to_vector<4>(llvm::map_range(ranges, [&](int64_t value) {
    return builder.getConstantAffineMap(value);
  }));
  SmallVector<int64_t> steps(ranges.size(), 1);
  build(builder, result, resultTypes, reductions, lbs, /*lbArgs=*/{}, ubs,
        /*ubArgs=*/{}, steps);
}

void AffineParallelOp::build(OpBuilder &builder, OperationState &result,
                             TypeRange resultTypes,
                             ArrayRef<arith::AtomicRMWKind> reductions,
                             ArrayRef<AffineMap> lbMaps, ValueRange lbArgs,
                             ArrayRef<AffineMap> ubMaps, ValueRange ubArgs,
                             ArrayRef<int64_t> steps) {
  assert(llvm::all_of(lbMaps,
                      [lbMaps](AffineMap m) {
                        return m.getNumDims() == lbMaps[0].getNumDims() &&
                               m.getNumSymbols() == lbMaps[0].getNumSymbols();
                      }) &&
         "expected all lower bounds maps to have the same number of dimensions "
         "and symbols");
  assert(llvm::all_of(ubMaps,
                      [ubMaps](AffineMap m) {
                        return m.getNumDims() == ubMaps[0].getNumDims() &&
                               m.getNumSymbols() == ubMaps[0].getNumSymbols();
                      }) &&
         "expected all upper bounds maps to have the same number of dimensions "
         "and symbols");
  assert((lbMaps.empty() || lbMaps[0].getNumInputs() == lbArgs.size()) &&
         "expected lower bound maps to have as many inputs as lower bound "
         "operands");
  assert((ubMaps.empty() || ubMaps[0].getNumInputs() == ubArgs.size()) &&
         "expected upper bound maps to have as many inputs as upper bound "
         "operands");

  result.addTypes(resultTypes);

  // Convert the reductions to integer attributes.
  SmallVector<Attribute, 4> reductionAttrs;
  for (arith::AtomicRMWKind reduction : reductions)
    reductionAttrs.push_back(
        builder.getI64IntegerAttr(static_cast<int64_t>(reduction)));
  result.addAttribute(getReductionsAttrStrName(),
                      builder.getArrayAttr(reductionAttrs));

  // Concatenates maps defined in the same input space (same dimensions and
  // symbols), assumes there is at least one map.
  auto concatMapsSameInput = [&builder](ArrayRef<AffineMap> maps,
                                        SmallVectorImpl<int32_t> &groups) {
    if (maps.empty())
      return AffineMap::get(builder.getContext());
    SmallVector<AffineExpr> exprs;
    groups.reserve(groups.size() + maps.size());
    exprs.reserve(maps.size());
    for (AffineMap m : maps) {
      llvm::append_range(exprs, m.getResults());
      groups.push_back(m.getNumResults());
    }
    return AffineMap::get(maps[0].getNumDims(), maps[0].getNumSymbols(), exprs,
                          maps[0].getContext());
  };

  // Set up the bounds.
  SmallVector<int32_t> lbGroups, ubGroups;
  AffineMap lbMap = concatMapsSameInput(lbMaps, lbGroups);
  AffineMap ubMap = concatMapsSameInput(ubMaps, ubGroups);
  result.addAttribute(getLowerBoundsMapAttrStrName(),
                      AffineMapAttr::get(lbMap));
  result.addAttribute(getLowerBoundsGroupsAttrStrName(),
                      builder.getI32TensorAttr(lbGroups));
  result.addAttribute(getUpperBoundsMapAttrStrName(),
                      AffineMapAttr::get(ubMap));
  result.addAttribute(getUpperBoundsGroupsAttrStrName(),
                      builder.getI32TensorAttr(ubGroups));
  result.addAttribute(getStepsAttrStrName(), builder.getI64ArrayAttr(steps));
  result.addOperands(lbArgs);
  result.addOperands(ubArgs);

  // Create a region and a block for the body.
  auto *bodyRegion = result.addRegion();
  auto *body = new Block();
  // Add all the block arguments.
  for (unsigned i = 0, e = steps.size(); i < e; ++i)
    body->addArgument(IndexType::get(builder.getContext()), result.location);
  bodyRegion->push_back(body);
  if (resultTypes.empty())
    ensureTerminator(*bodyRegion, builder, result.location);
}

Region &AffineParallelOp::getLoopBody() { return getRegion(); }

unsigned AffineParallelOp::getNumDims() { return getSteps().size(); }

AffineParallelOp::operand_range AffineParallelOp::getLowerBoundsOperands() {
  return getOperands().take_front(getLowerBoundsMap().getNumInputs());
}

AffineParallelOp::operand_range AffineParallelOp::getUpperBoundsOperands() {
  return getOperands().drop_front(getLowerBoundsMap().getNumInputs());
}

AffineMap AffineParallelOp::getLowerBoundMap(unsigned pos) {
  auto values = getLowerBoundsGroups().getValues<int32_t>();
  unsigned start = 0;
  for (unsigned i = 0; i < pos; ++i)
    start += values[i];
  return getLowerBoundsMap().getSliceMap(start, values[pos]);
}

AffineMap AffineParallelOp::getUpperBoundMap(unsigned pos) {
  auto values = getUpperBoundsGroups().getValues<int32_t>();
  unsigned start = 0;
  for (unsigned i = 0; i < pos; ++i)
    start += values[i];
  return getUpperBoundsMap().getSliceMap(start, values[pos]);
}

AffineValueMap AffineParallelOp::getLowerBoundsValueMap() {
  return AffineValueMap(getLowerBoundsMap(), getLowerBoundsOperands());
}

AffineValueMap AffineParallelOp::getUpperBoundsValueMap() {
  return AffineValueMap(getUpperBoundsMap(), getUpperBoundsOperands());
}

std::optional<SmallVector<int64_t, 8>> AffineParallelOp::getConstantRanges() {
  if (hasMinMaxBounds())
    return std::nullopt;

  // Try to convert all the ranges to constant expressions.
  SmallVector<int64_t, 8> out;
  AffineValueMap rangesValueMap;
  AffineValueMap::difference(getUpperBoundsValueMap(), getLowerBoundsValueMap(),
                             &rangesValueMap);
  out.reserve(rangesValueMap.getNumResults());
  for (unsigned i = 0, e = rangesValueMap.getNumResults(); i < e; ++i) {
    auto expr = rangesValueMap.getResult(i);
    auto cst = expr.dyn_cast<AffineConstantExpr>();
    if (!cst)
      return std::nullopt;
    out.push_back(cst.getValue());
  }
  return out;
}

Block *AffineParallelOp::getBody() { return &getRegion().front(); }

OpBuilder AffineParallelOp::getBodyBuilder() {
  return OpBuilder(getBody(), std::prev(getBody()->end()));
}

void AffineParallelOp::setLowerBounds(ValueRange lbOperands, AffineMap map) {
  assert(lbOperands.size() == map.getNumInputs() &&
         "operands to map must match number of inputs");

  auto ubOperands = getUpperBoundsOperands();

  SmallVector<Value, 4> newOperands(lbOperands);
  newOperands.append(ubOperands.begin(), ubOperands.end());
  (*this)->setOperands(newOperands);

  setLowerBoundsMapAttr(AffineMapAttr::get(map));
}

void AffineParallelOp::setUpperBounds(ValueRange ubOperands, AffineMap map) {
  assert(ubOperands.size() == map.getNumInputs() &&
         "operands to map must match number of inputs");

  SmallVector<Value, 4> newOperands(getLowerBoundsOperands());
  newOperands.append(ubOperands.begin(), ubOperands.end());
  (*this)->setOperands(newOperands);

  setUpperBoundsMapAttr(AffineMapAttr::get(map));
}

void AffineParallelOp::setSteps(ArrayRef<int64_t> newSteps) {
  setStepsAttr(getBodyBuilder().getI64ArrayAttr(newSteps));
}

LogicalResult AffineParallelOp::verify() {
  auto numDims = getNumDims();
  if (getLowerBoundsGroups().getNumElements() != numDims ||
      getUpperBoundsGroups().getNumElements() != numDims ||
      getSteps().size() != numDims || getBody()->getNumArguments() != numDims) {
    return emitOpError() << "the number of region arguments ("
                         << getBody()->getNumArguments()
                         << ") and the number of map groups for lower ("
                         << getLowerBoundsGroups().getNumElements()
                         << ") and upper bound ("
                         << getUpperBoundsGroups().getNumElements()
                         << "), and the number of steps (" << getSteps().size()
                         << ") must all match";
  }

  unsigned expectedNumLBResults = 0;
  for (APInt v : getLowerBoundsGroups())
    expectedNumLBResults += v.getZExtValue();
  if (expectedNumLBResults != getLowerBoundsMap().getNumResults())
    return emitOpError() << "expected lower bounds map to have "
                         << expectedNumLBResults << " results";
  unsigned expectedNumUBResults = 0;
  for (APInt v : getUpperBoundsGroups())
    expectedNumUBResults += v.getZExtValue();
  if (expectedNumUBResults != getUpperBoundsMap().getNumResults())
    return emitOpError() << "expected upper bounds map to have "
                         << expectedNumUBResults << " results";

  if (getReductions().size() != getNumResults())
    return emitOpError("a reduction must be specified for each output");

  // Verify reduction  ops are all valid
  for (Attribute attr : getReductions()) {
    auto intAttr = llvm::dyn_cast<IntegerAttr>(attr);
    if (!intAttr || !arith::symbolizeAtomicRMWKind(intAttr.getInt()))
      return emitOpError("invalid reduction attribute");
  }

  // Verify that the bound operands are valid dimension/symbols.
  /// Lower bounds.
  if (failed(verifyDimAndSymbolIdentifiers(*this, getLowerBoundsOperands(),
                                           getLowerBoundsMap().getNumDims())))
    return failure();
  /// Upper bounds.
  if (failed(verifyDimAndSymbolIdentifiers(*this, getUpperBoundsOperands(),
                                           getUpperBoundsMap().getNumDims())))
    return failure();
  return success();
}

LogicalResult AffineValueMap::canonicalize() {
  SmallVector<Value, 4> newOperands{operands};
  auto newMap = getAffineMap();
  composeAffineMapAndOperands(&newMap, &newOperands);
  if (newMap == getAffineMap() && newOperands == operands)
    return failure();
  reset(newMap, newOperands);
  return success();
}

/// Canonicalize the bounds of the given loop.
static LogicalResult canonicalizeLoopBounds(AffineParallelOp op) {
  AffineValueMap lb = op.getLowerBoundsValueMap();
  bool lbCanonicalized = succeeded(lb.canonicalize());

  AffineValueMap ub = op.getUpperBoundsValueMap();
  bool ubCanonicalized = succeeded(ub.canonicalize());

  // Any canonicalization change always leads to updated map(s).
  if (!lbCanonicalized && !ubCanonicalized)
    return failure();

  if (lbCanonicalized)
    op.setLowerBounds(lb.getOperands(), lb.getAffineMap());
  if (ubCanonicalized)
    op.setUpperBounds(ub.getOperands(), ub.getAffineMap());

  return success();
}

LogicalResult AffineParallelOp::fold(FoldAdaptor adaptor,
                                     SmallVectorImpl<OpFoldResult> &results) {
  return canonicalizeLoopBounds(*this);
}

/// Prints a lower(upper) bound of an affine parallel loop with max(min)
/// conditions in it. `mapAttr` is a flat list of affine expressions and `group`
/// identifies which of the those expressions form max/min groups. `operands`
/// are the SSA values of dimensions and symbols and `keyword` is either "min"
/// or "max".
static void printMinMaxBound(OpAsmPrinter &p, AffineMapAttr mapAttr,
                             DenseIntElementsAttr group, ValueRange operands,
                             StringRef keyword) {
  AffineMap map = mapAttr.getValue();
  unsigned numDims = map.getNumDims();
  ValueRange dimOperands = operands.take_front(numDims);
  ValueRange symOperands = operands.drop_front(numDims);
  unsigned start = 0;
  for (llvm::APInt groupSize : group) {
    if (start != 0)
      p << ", ";

    unsigned size = groupSize.getZExtValue();
    if (size == 1) {
      p.printAffineExprOfSSAIds(map.getResult(start), dimOperands, symOperands);
      ++start;
    } else {
      p << keyword << '(';
      AffineMap submap = map.getSliceMap(start, size);
      p.printAffineMapOfSSAIds(AffineMapAttr::get(submap), operands);
      p << ')';
      start += size;
    }
  }
}

void AffineParallelOp::print(OpAsmPrinter &p) {
  p << " (" << getBody()->getArguments() << ") = (";
  printMinMaxBound(p, getLowerBoundsMapAttr(), getLowerBoundsGroupsAttr(),
                   getLowerBoundsOperands(), "max");
  p << ") to (";
  printMinMaxBound(p, getUpperBoundsMapAttr(), getUpperBoundsGroupsAttr(),
                   getUpperBoundsOperands(), "min");
  p << ')';
  SmallVector<int64_t, 8> steps = getSteps();
  bool elideSteps = llvm::all_of(steps, [](int64_t step) { return step == 1; });
  if (!elideSteps) {
    p << " step (";
    llvm::interleaveComma(steps, p);
    p << ')';
  }
  if (getNumResults()) {
    p << " reduce (";
    llvm::interleaveComma(getReductions(), p, [&](auto &attr) {
      arith::AtomicRMWKind sym = *arith::symbolizeAtomicRMWKind(
          llvm::cast<IntegerAttr>(attr).getInt());
      p << "\"" << arith::stringifyAtomicRMWKind(sym) << "\"";
    });
    p << ") -> (" << getResultTypes() << ")";
  }

  p << ' ';
  p.printRegion(getRegion(), /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/getNumResults());
  p.printOptionalAttrDict(
      (*this)->getAttrs(),
      /*elidedAttrs=*/{AffineParallelOp::getReductionsAttrStrName(),
                       AffineParallelOp::getLowerBoundsMapAttrStrName(),
                       AffineParallelOp::getLowerBoundsGroupsAttrStrName(),
                       AffineParallelOp::getUpperBoundsMapAttrStrName(),
                       AffineParallelOp::getUpperBoundsGroupsAttrStrName(),
                       AffineParallelOp::getStepsAttrStrName()});
}

/// Given a list of lists of parsed operands, populates `uniqueOperands` with
/// unique operands. Also populates `replacements with affine expressions of
/// `kind` that can be used to update affine maps previously accepting a
/// `operands` to accept `uniqueOperands` instead.
static ParseResult deduplicateAndResolveOperands(
    OpAsmParser &parser,
    ArrayRef<SmallVector<OpAsmParser::UnresolvedOperand>> operands,
    SmallVectorImpl<Value> &uniqueOperands,
    SmallVectorImpl<AffineExpr> &replacements, AffineExprKind kind) {
  assert((kind == AffineExprKind::DimId || kind == AffineExprKind::SymbolId) &&
         "expected operands to be dim or symbol expression");

  Type indexType = parser.getBuilder().getIndexType();
  for (const auto &list : operands) {
    SmallVector<Value> valueOperands;
    if (parser.resolveOperands(list, indexType, valueOperands))
      return failure();
    for (Value operand : valueOperands) {
      unsigned pos = std::distance(uniqueOperands.begin(),
                                   llvm::find(uniqueOperands, operand));
      if (pos == uniqueOperands.size())
        uniqueOperands.push_back(operand);
      replacements.push_back(
          kind == AffineExprKind::DimId
              ? getAffineDimExpr(pos, parser.getContext())
              : getAffineSymbolExpr(pos, parser.getContext()));
    }
  }
  return success();
}

namespace {
enum class MinMaxKind { Min, Max };
} // namespace

/// Parses an affine map that can contain a min/max for groups of its results,
/// e.g., max(expr-1, expr-2), expr-3, max(expr-4, expr-5, expr-6). Populates
/// `result` attributes with the map (flat list of expressions) and the grouping
/// (list of integers that specify how many expressions to put into each
/// min/max) attributes. Deduplicates repeated operands.
///
/// parallel-bound       ::= `(` parallel-group-list `)`
/// parallel-group-list  ::= parallel-group (`,` parallel-group-list)?
/// parallel-group       ::= simple-group | min-max-group
/// simple-group         ::= expr-of-ssa-ids
/// min-max-group        ::= ( `min` | `max` ) `(` expr-of-ssa-ids-list `)`
/// expr-of-ssa-ids-list ::= expr-of-ssa-ids (`,` expr-of-ssa-id-list)?
///
/// Examples:
///   (%0, min(%1 + %2, %3), %4, min(%5 floordiv 32, %6))
///   (%0, max(%1 - 2 * %2))
static ParseResult parseAffineMapWithMinMax(OpAsmParser &parser,
                                            OperationState &result,
                                            MinMaxKind kind) {
  // Using `const` not `constexpr` below to workaround a MSVC optimizer bug,
  // see: https://reviews.llvm.org/D134227#3821753
  const llvm::StringLiteral tmpAttrStrName = "__pseudo_bound_map";

  StringRef mapName = kind == MinMaxKind::Min
                          ? AffineParallelOp::getUpperBoundsMapAttrStrName()
                          : AffineParallelOp::getLowerBoundsMapAttrStrName();
  StringRef groupsName =
      kind == MinMaxKind::Min
          ? AffineParallelOp::getUpperBoundsGroupsAttrStrName()
          : AffineParallelOp::getLowerBoundsGroupsAttrStrName();

  if (failed(parser.parseLParen()))
    return failure();

  if (succeeded(parser.parseOptionalRParen())) {
    result.addAttribute(
        mapName, AffineMapAttr::get(parser.getBuilder().getEmptyAffineMap()));
    result.addAttribute(groupsName, parser.getBuilder().getI32TensorAttr({}));
    return success();
  }

  SmallVector<AffineExpr> flatExprs;
  SmallVector<SmallVector<OpAsmParser::UnresolvedOperand>> flatDimOperands;
  SmallVector<SmallVector<OpAsmParser::UnresolvedOperand>> flatSymOperands;
  SmallVector<int32_t> numMapsPerGroup;
  SmallVector<OpAsmParser::UnresolvedOperand> mapOperands;
  auto parseOperands = [&]() {
    if (succeeded(parser.parseOptionalKeyword(
            kind == MinMaxKind::Min ? "min" : "max"))) {
      mapOperands.clear();
      AffineMapAttr map;
      if (failed(parser.parseAffineMapOfSSAIds(mapOperands, map, tmpAttrStrName,
                                               result.attributes,
                                               OpAsmParser::Delimiter::Paren)))
        return failure();
      result.attributes.erase(tmpAttrStrName);
      llvm::append_range(flatExprs, map.getValue().getResults());
      auto operandsRef = llvm::ArrayRef(mapOperands);
      auto dimsRef = operandsRef.take_front(map.getValue().getNumDims());
      SmallVector<OpAsmParser::UnresolvedOperand> dims(dimsRef.begin(),
                                                       dimsRef.end());
      auto symsRef = operandsRef.drop_front(map.getValue().getNumDims());
      SmallVector<OpAsmParser::UnresolvedOperand> syms(symsRef.begin(),
                                                       symsRef.end());
      flatDimOperands.append(map.getValue().getNumResults(), dims);
      flatSymOperands.append(map.getValue().getNumResults(), syms);
      numMapsPerGroup.push_back(map.getValue().getNumResults());
    } else {
      if (failed(parser.parseAffineExprOfSSAIds(flatDimOperands.emplace_back(),
                                                flatSymOperands.emplace_back(),
                                                flatExprs.emplace_back())))
        return failure();
      numMapsPerGroup.push_back(1);
    }
    return success();
  };
  if (parser.parseCommaSeparatedList(parseOperands) || parser.parseRParen())
    return failure();

  unsigned totalNumDims = 0;
  unsigned totalNumSyms = 0;
  for (unsigned i = 0, e = flatExprs.size(); i < e; ++i) {
    unsigned numDims = flatDimOperands[i].size();
    unsigned numSyms = flatSymOperands[i].size();
    flatExprs[i] = flatExprs[i]
                       .shiftDims(numDims, totalNumDims)
                       .shiftSymbols(numSyms, totalNumSyms);
    totalNumDims += numDims;
    totalNumSyms += numSyms;
  }

  // Deduplicate map operands.
  SmallVector<Value> dimOperands, symOperands;
  SmallVector<AffineExpr> dimRplacements, symRepacements;
  if (deduplicateAndResolveOperands(parser, flatDimOperands, dimOperands,
                                    dimRplacements, AffineExprKind::DimId) ||
      deduplicateAndResolveOperands(parser, flatSymOperands, symOperands,
                                    symRepacements, AffineExprKind::SymbolId))
    return failure();

  result.operands.append(dimOperands.begin(), dimOperands.end());
  result.operands.append(symOperands.begin(), symOperands.end());

  Builder &builder = parser.getBuilder();
  auto flatMap = AffineMap::get(totalNumDims, totalNumSyms, flatExprs,
                                parser.getContext());
  flatMap = flatMap.replaceDimsAndSymbols(
      dimRplacements, symRepacements, dimOperands.size(), symOperands.size());

  result.addAttribute(mapName, AffineMapAttr::get(flatMap));
  result.addAttribute(groupsName, builder.getI32TensorAttr(numMapsPerGroup));
  return success();
}

//
// operation ::= `affine.parallel` `(` ssa-ids `)` `=` parallel-bound
//               `to` parallel-bound steps? region attr-dict?
// steps     ::= `steps` `(` integer-literals `)`
//
ParseResult AffineParallelOp::parse(OpAsmParser &parser,
                                    OperationState &result) {
  auto &builder = parser.getBuilder();
  auto indexType = builder.getIndexType();
  SmallVector<OpAsmParser::Argument, 4> ivs;
  if (parser.parseArgumentList(ivs, OpAsmParser::Delimiter::Paren) ||
      parser.parseEqual() ||
      parseAffineMapWithMinMax(parser, result, MinMaxKind::Max) ||
      parser.parseKeyword("to") ||
      parseAffineMapWithMinMax(parser, result, MinMaxKind::Min))
    return failure();

  AffineMapAttr stepsMapAttr;
  NamedAttrList stepsAttrs;
  SmallVector<OpAsmParser::UnresolvedOperand, 4> stepsMapOperands;
  if (failed(parser.parseOptionalKeyword("step"))) {
    SmallVector<int64_t, 4> steps(ivs.size(), 1);
    result.addAttribute(AffineParallelOp::getStepsAttrStrName(),
                        builder.getI64ArrayAttr(steps));
  } else {
    if (parser.parseAffineMapOfSSAIds(stepsMapOperands, stepsMapAttr,
                                      AffineParallelOp::getStepsAttrStrName(),
                                      stepsAttrs,
                                      OpAsmParser::Delimiter::Paren))
      return failure();

    // Convert steps from an AffineMap into an I64ArrayAttr.
    SmallVector<int64_t, 4> steps;
    auto stepsMap = stepsMapAttr.getValue();
    for (const auto &result : stepsMap.getResults()) {
      auto constExpr = result.dyn_cast<AffineConstantExpr>();
      if (!constExpr)
        return parser.emitError(parser.getNameLoc(),
                                "steps must be constant integers");
      steps.push_back(constExpr.getValue());
    }
    result.addAttribute(AffineParallelOp::getStepsAttrStrName(),
                        builder.getI64ArrayAttr(steps));
  }

  // Parse optional clause of the form: `reduce ("addf", "maxf")`, where the
  // quoted strings are a member of the enum AtomicRMWKind.
  SmallVector<Attribute, 4> reductions;
  if (succeeded(parser.parseOptionalKeyword("reduce"))) {
    if (parser.parseLParen())
      return failure();
    auto parseAttributes = [&]() -> ParseResult {
      // Parse a single quoted string via the attribute parsing, and then
      // verify it is a member of the enum and convert to it's integer
      // representation.
      StringAttr attrVal;
      NamedAttrList attrStorage;
      auto loc = parser.getCurrentLocation();
      if (parser.parseAttribute(attrVal, builder.getNoneType(), "reduce",
                                attrStorage))
        return failure();
      std::optional<arith::AtomicRMWKind> reduction =
          arith::symbolizeAtomicRMWKind(attrVal.getValue());
      if (!reduction)
        return parser.emitError(loc, "invalid reduction value: ") << attrVal;
      reductions.push_back(
          builder.getI64IntegerAttr(static_cast<int64_t>(reduction.value())));
      // While we keep getting commas, keep parsing.
      return success();
    };
    if (parser.parseCommaSeparatedList(parseAttributes) || parser.parseRParen())
      return failure();
  }
  result.addAttribute(AffineParallelOp::getReductionsAttrStrName(),
                      builder.getArrayAttr(reductions));

  // Parse return types of reductions (if any)
  if (parser.parseOptionalArrowTypeList(result.types))
    return failure();

  // Now parse the body.
  Region *body = result.addRegion();
  for (auto &iv : ivs)
    iv.type = indexType;
  if (parser.parseRegion(*body, ivs) ||
      parser.parseOptionalAttrDict(result.attributes))
    return failure();

  // Add a terminator if none was parsed.
  AffineParallelOp::ensureTerminator(*body, builder, result.location);
  return success();
}

//===----------------------------------------------------------------------===//
// AffineYieldOp
//===----------------------------------------------------------------------===//

LogicalResult AffineYieldOp::verify() {
  auto *parentOp = (*this)->getParentOp();
  auto results = parentOp->getResults();
  auto operands = getOperands();

  if (!isa<AffineParallelOp, AffineIfOp, AffineForOp>(parentOp))
    return emitOpError() << "only terminates affine.if/for/parallel regions";
  if (parentOp->getNumResults() != getNumOperands())
    return emitOpError() << "parent of yield must have same number of "
                            "results as the yield operands";
  for (auto it : llvm::zip(results, operands)) {
    if (std::get<0>(it).getType() != std::get<1>(it).getType())
      return emitOpError() << "types mismatch between yield op and its parent";
  }

  return success();
}

//===----------------------------------------------------------------------===//
// AffineVectorLoadOp
//===----------------------------------------------------------------------===//

void AffineVectorLoadOp::build(OpBuilder &builder, OperationState &result,
                               VectorType resultType, AffineMap map,
                               ValueRange operands) {
  assert(operands.size() == 1 + map.getNumInputs() && "inconsistent operands");
  result.addOperands(operands);
  if (map)
    result.addAttribute(getMapAttrStrName(), AffineMapAttr::get(map));
  result.types.push_back(resultType);
}

void AffineVectorLoadOp::build(OpBuilder &builder, OperationState &result,
                               VectorType resultType, Value memref,
                               AffineMap map, ValueRange mapOperands) {
  assert(map.getNumInputs() == mapOperands.size() && "inconsistent index info");
  result.addOperands(memref);
  result.addOperands(mapOperands);
  result.addAttribute(getMapAttrStrName(), AffineMapAttr::get(map));
  result.types.push_back(resultType);
}

void AffineVectorLoadOp::build(OpBuilder &builder, OperationState &result,
                               VectorType resultType, Value memref,
                               ValueRange indices) {
  auto memrefType = llvm::cast<MemRefType>(memref.getType());
  int64_t rank = memrefType.getRank();
  // Create identity map for memrefs with at least one dimension or () -> ()
  // for zero-dimensional memrefs.
  auto map =
      rank ? builder.getMultiDimIdentityMap(rank) : builder.getEmptyAffineMap();
  build(builder, result, resultType, memref, map, indices);
}

void AffineVectorLoadOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                     MLIRContext *context) {
  results.add<SimplifyAffineOp<AffineVectorLoadOp>>(context);
}

ParseResult AffineVectorLoadOp::parse(OpAsmParser &parser,
                                      OperationState &result) {
  auto &builder = parser.getBuilder();
  auto indexTy = builder.getIndexType();

  MemRefType memrefType;
  VectorType resultType;
  OpAsmParser::UnresolvedOperand memrefInfo;
  AffineMapAttr mapAttr;
  SmallVector<OpAsmParser::UnresolvedOperand, 1> mapOperands;
  return failure(
      parser.parseOperand(memrefInfo) ||
      parser.parseAffineMapOfSSAIds(mapOperands, mapAttr,
                                    AffineVectorLoadOp::getMapAttrStrName(),
                                    result.attributes) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(memrefType) || parser.parseComma() ||
      parser.parseType(resultType) ||
      parser.resolveOperand(memrefInfo, memrefType, result.operands) ||
      parser.resolveOperands(mapOperands, indexTy, result.operands) ||
      parser.addTypeToList(resultType, result.types));
}

void AffineVectorLoadOp::print(OpAsmPrinter &p) {
  p << " " << getMemRef() << '[';
  if (AffineMapAttr mapAttr =
          (*this)->getAttrOfType<AffineMapAttr>(getMapAttrStrName()))
    p.printAffineMapOfSSAIds(mapAttr, getMapOperands());
  p << ']';
  p.printOptionalAttrDict((*this)->getAttrs(),
                          /*elidedAttrs=*/{getMapAttrStrName()});
  p << " : " << getMemRefType() << ", " << getType();
}

/// Verify common invariants of affine.vector_load and affine.vector_store.
static LogicalResult verifyVectorMemoryOp(Operation *op, MemRefType memrefType,
                                          VectorType vectorType) {
  // Check that memref and vector element types match.
  if (memrefType.getElementType() != vectorType.getElementType())
    return op->emitOpError(
        "requires memref and vector types of the same elemental type");
  return success();
}

LogicalResult AffineVectorLoadOp::verify() {
  MemRefType memrefType = getMemRefType();
  if (failed(verifyMemoryOpIndexing(
          getOperation(),
          (*this)->getAttrOfType<AffineMapAttr>(getMapAttrStrName()),
          getMapOperands(), memrefType,
          /*numIndexOperands=*/getNumOperands() - 1)))
    return failure();

  if (failed(verifyVectorMemoryOp(getOperation(), memrefType, getVectorType())))
    return failure();

  return success();
}

//===----------------------------------------------------------------------===//
// AffineVectorStoreOp
//===----------------------------------------------------------------------===//

void AffineVectorStoreOp::build(OpBuilder &builder, OperationState &result,
                                Value valueToStore, Value memref, AffineMap map,
                                ValueRange mapOperands) {
  assert(map.getNumInputs() == mapOperands.size() && "inconsistent index info");
  result.addOperands(valueToStore);
  result.addOperands(memref);
  result.addOperands(mapOperands);
  result.addAttribute(getMapAttrStrName(), AffineMapAttr::get(map));
}

// Use identity map.
void AffineVectorStoreOp::build(OpBuilder &builder, OperationState &result,
                                Value valueToStore, Value memref,
                                ValueRange indices) {
  auto memrefType = llvm::cast<MemRefType>(memref.getType());
  int64_t rank = memrefType.getRank();
  // Create identity map for memrefs with at least one dimension or () -> ()
  // for zero-dimensional memrefs.
  auto map =
      rank ? builder.getMultiDimIdentityMap(rank) : builder.getEmptyAffineMap();
  build(builder, result, valueToStore, memref, map, indices);
}
void AffineVectorStoreOp::getCanonicalizationPatterns(
    RewritePatternSet &results, MLIRContext *context) {
  results.add<SimplifyAffineOp<AffineVectorStoreOp>>(context);
}

ParseResult AffineVectorStoreOp::parse(OpAsmParser &parser,
                                       OperationState &result) {
  auto indexTy = parser.getBuilder().getIndexType();

  MemRefType memrefType;
  VectorType resultType;
  OpAsmParser::UnresolvedOperand storeValueInfo;
  OpAsmParser::UnresolvedOperand memrefInfo;
  AffineMapAttr mapAttr;
  SmallVector<OpAsmParser::UnresolvedOperand, 1> mapOperands;
  return failure(
      parser.parseOperand(storeValueInfo) || parser.parseComma() ||
      parser.parseOperand(memrefInfo) ||
      parser.parseAffineMapOfSSAIds(mapOperands, mapAttr,
                                    AffineVectorStoreOp::getMapAttrStrName(),
                                    result.attributes) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(memrefType) || parser.parseComma() ||
      parser.parseType(resultType) ||
      parser.resolveOperand(storeValueInfo, resultType, result.operands) ||
      parser.resolveOperand(memrefInfo, memrefType, result.operands) ||
      parser.resolveOperands(mapOperands, indexTy, result.operands));
}

void AffineVectorStoreOp::print(OpAsmPrinter &p) {
  p << " " << getValueToStore();
  p << ", " << getMemRef() << '[';
  if (AffineMapAttr mapAttr =
          (*this)->getAttrOfType<AffineMapAttr>(getMapAttrStrName()))
    p.printAffineMapOfSSAIds(mapAttr, getMapOperands());
  p << ']';
  p.printOptionalAttrDict((*this)->getAttrs(),
                          /*elidedAttrs=*/{getMapAttrStrName()});
  p << " : " << getMemRefType() << ", " << getValueToStore().getType();
}

LogicalResult AffineVectorStoreOp::verify() {
  MemRefType memrefType = getMemRefType();
  if (failed(verifyMemoryOpIndexing(
          *this, (*this)->getAttrOfType<AffineMapAttr>(getMapAttrStrName()),
          getMapOperands(), memrefType,
          /*numIndexOperands=*/getNumOperands() - 2)))
    return failure();

  if (failed(verifyVectorMemoryOp(*this, memrefType, getVectorType())))
    return failure();

  return success();
}

//===----------------------------------------------------------------------===//
// DelinearizeIndexOp
//===----------------------------------------------------------------------===//

void AffineDelinearizeIndexOp::build(OpBuilder &builder, OperationState &result,
                                     Value linearIndex,
                                     ArrayRef<OpFoldResult> basis) {
  result.addTypes(SmallVector<Type>(basis.size(), builder.getIndexType()));
  result.addOperands(linearIndex);
  SmallVector<Value> basisValues =
      llvm::map_to_vector(basis, [&](OpFoldResult ofr) -> Value {
        std::optional<int64_t> staticDim = getConstantIntValue(ofr);
        if (staticDim.has_value())
          return builder.create<arith::ConstantIndexOp>(result.location,
                                                        *staticDim);
        return llvm::dyn_cast_if_present<Value>(ofr);
      });
  result.addOperands(basisValues);
}

LogicalResult AffineDelinearizeIndexOp::verify() {
  if (getBasis().empty())
    return emitOpError("basis should not be empty");
  if (getNumResults() != getBasis().size())
    return emitOpError("should return an index for each basis element");
  return success();
}

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "mlir/Dialect/Affine/IR/AffineOps.cpp.inc"
