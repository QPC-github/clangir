//===-- CIRGenBuilder.h - CIRBuilder implementation  ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_CIR_CIRGENBUILDER_H
#define LLVM_CLANG_LIB_CIR_CIRGENBUILDER_H

#include "Address.h"
#include "UnimplementedFeatureGuarding.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/IR/FPEnv.h"

#include "mlir/IR/Builders.h"
#include "llvm/ADT/FloatingPointMode.h"

namespace cir {

class CIRGenFunction;

class CIRGenBuilderTy : public mlir::OpBuilder {
  bool IsFPConstrained = false;
  fp::ExceptionBehavior DefaultConstrainedExcept = fp::ebStrict;
  llvm::RoundingMode DefaultConstrainedRounding = llvm::RoundingMode::Dynamic;

public:
  CIRGenBuilderTy(mlir::MLIRContext &C) : mlir::OpBuilder(&C) {}

  //
  // Floating point specific helpers
  // -------------------------------
  //

  /// Enable/Disable use of constrained floating point math. When enabled the
  /// CreateF<op>() calls instead create constrained floating point intrinsic
  /// calls. Fast math flags are unaffected by this setting.
  void setIsFPConstrained(bool IsCon) {
    if (IsCon)
      llvm_unreachable("Constrained FP NYI");
    IsFPConstrained = IsCon;
  }

  /// Query for the use of constrained floating point math
  bool getIsFPConstrained() {
    if (IsFPConstrained)
      llvm_unreachable("Constrained FP NYI");
    return IsFPConstrained;
  }

  /// Set the exception handling to be used with constrained floating point
  void setDefaultConstrainedExcept(fp::ExceptionBehavior NewExcept) {
#ifndef NDEBUG
    std::optional<llvm::StringRef> ExceptStr =
        convertExceptionBehaviorToStr(NewExcept);
    assert(ExceptStr && "Garbage strict exception behavior!");
#endif
    DefaultConstrainedExcept = NewExcept;
  }

  /// Set the rounding mode handling to be used with constrained floating point
  void setDefaultConstrainedRounding(llvm::RoundingMode NewRounding) {
#ifndef NDEBUG
    std::optional<llvm::StringRef> RoundingStr =
        convertRoundingModeToStr(NewRounding);
    assert(RoundingStr && "Garbage strict rounding mode!");
#endif
    DefaultConstrainedRounding = NewRounding;
  }

  /// Get the exception handling used with constrained floating point
  fp::ExceptionBehavior getDefaultConstrainedExcept() {
    return DefaultConstrainedExcept;
  }

  /// Get the rounding mode handling used with constrained floating point
  llvm::RoundingMode getDefaultConstrainedRounding() {
    return DefaultConstrainedRounding;
  }

  //
  // Type helpers
  // ------------
  //

  // Fetch the type representing a pointer to an 8-bit integer value.
  mlir::cir::PointerType getInt8PtrTy(unsigned AddrSpace = 0) {
    return mlir::cir::PointerType::get(getContext(),
                                       mlir::IntegerType::get(getContext(), 8));
  }

  // Get a constant 32-bit value.
  mlir::cir::ConstantOp getInt32(uint32_t C, mlir::Location loc) {
    auto int32Ty = mlir::IntegerType::get(getContext(), 32);
    return create<mlir::cir::ConstantOp>(loc, int32Ty,
                                         mlir::IntegerAttr::get(int32Ty, C));
  }

  // Get a bool
  mlir::Value getBool(bool state, mlir::Location loc) {
    return create<mlir::cir::ConstantOp>(
        loc, getBoolTy(), mlir::BoolAttr::get(getContext(), state));
  }

  // Get the bool type
  mlir::cir::BoolType getBoolTy() {
    return ::mlir::cir::BoolType::get(getContext());
  }

  // Creates constant pointer for type ty.
  mlir::cir::ConstantOp getNullPtr(mlir::Type ty, mlir::Location loc) {
    assert(ty.isa<mlir::cir::PointerType>() && "expected cir.ptr");
    return create<mlir::cir::ConstantOp>(
        loc, ty, mlir::cir::NullAttr::get(getContext(), ty));
  }

  // Creates null value for type ty.
  mlir::cir::ConstantOp getNullValue(mlir::Type ty, mlir::Location loc) {
    assert(ty.isa<mlir::IntegerType>() && "NYI");
    return create<mlir::cir::ConstantOp>(loc, ty,
                                         mlir::IntegerAttr::get(ty, 0));
  }

  mlir::Value getBitcast(mlir::Location loc, mlir::Value src,
                         mlir::Type newTy) {
    if (newTy == src.getType())
      return src;
    return create<mlir::cir::CastOp>(loc, newTy, mlir::cir::CastKind::bitcast,
                                     src);
  }

  mlir::cir::PointerType getPointerTo(mlir::Type ty,
                                      unsigned addressSpace = 0) {
    assert(!UnimplementedFeature::addressSpace() && "NYI");
    return mlir::cir::PointerType::get(getContext(), ty);
  }

  /// Cast the element type of the given address to a different type,
  /// preserving information like the alignment.
  Address getElementBitCast(mlir::Location loc, Address Addr, mlir::Type Ty) {
    assert(!UnimplementedFeature::addressSpace() && "NYI");
    auto ptrTy = getPointerTo(Ty);
    return Address(getBitcast(loc, Addr.getPointer(), ptrTy), Ty,
                   Addr.getAlignment());
  }

  OpBuilder::InsertPoint getBestAllocaInsertPoint(mlir::Block *block) {
    auto lastAlloca =
        std::find_if(block->rbegin(), block->rend(), [](mlir::Operation &op) {
          return mlir::isa<mlir::cir::AllocaOp>(&op);
        });

    if (lastAlloca != block->rend())
      return OpBuilder::InsertPoint(block,
                                    ++mlir::Block::iterator(&*lastAlloca));
    return OpBuilder::InsertPoint(block, block->begin());
  };

  //
  // Operation creation helpers
  // --------------------------
  //

  mlir::Value createFPExt(mlir::Value v, mlir::Type destType) {
    if (getIsFPConstrained())
      llvm_unreachable("constrainedfp NYI");

    return create<mlir::cir::CastOp>(v.getLoc(), destType,
                                     mlir::cir::CastKind::floating, v);
  }

  cir::Address createElementBitCast(mlir::Location loc, cir::Address addr,
                                    mlir::Type destType) {
    if (destType == addr.getElementType())
      return addr;

    auto newPtrType = mlir::cir::PointerType::get(getContext(), destType);
    auto cast = getBitcast(loc, addr.getPointer(), newPtrType);
    return Address(cast, addr.getElementType(), addr.getAlignment());
  }

  mlir::Value createLoad(mlir::Location loc, Address addr) {
    return create<mlir::cir::LoadOp>(loc, addr.getElementType(),
                                     addr.getPointer());
  }

  mlir::cir::StoreOp createStore(mlir::Location loc, mlir::Value val,
                                 Address dst) {
    return create<mlir::cir::StoreOp>(loc, val, dst.getPointer());
  }
};

} // namespace cir

#endif
