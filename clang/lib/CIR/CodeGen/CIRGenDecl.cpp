//===--- CIRGenDecl.cpp - Emit CIR Code for declarations ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This contains code to emit Decl nodes as CIR code.
//
//===----------------------------------------------------------------------===//

#include "CIRGenCstEmitter.h"
#include "CIRGenFunction.h"

#include "clang/AST/Decl.h"

using namespace cir;
using namespace clang;

CIRGenFunction::AutoVarEmission
CIRGenFunction::buildAutoVarAlloca(const VarDecl &D) {
  QualType Ty = D.getType();
  // TODO: (|| Ty.getAddressSpace() == LangAS::opencl_private &&
  //        getLangOpts().OpenCL))
  assert(!UnimplementedFeature::openCL());
  assert(!UnimplementedFeature::openMP());
  assert(Ty.getAddressSpace() == LangAS::Default);
  assert(!Ty->isVariablyModifiedType() && "not implemented");
  assert(!getContext()
              .getLangOpts()
              .OpenMP && // !CGF.getLangOpts().OpenMPIRBuilder
         "not implemented");
  assert(!D.hasAttr<AnnotateAttr>() && "not implemented");

  bool NRVO =
      getContext().getLangOpts().ElideConstructors && D.isNRVOVariable();
  AutoVarEmission emission(D);
  bool isEscapingByRef = D.isEscapingByref();
  emission.IsEscapingByRef = isEscapingByRef;

  CharUnits alignment = getContext().getDeclAlign(&D);
  assert(!UnimplementedFeature::generateDebugInfo());
  assert(!UnimplementedFeature::cxxABI());

  Address address = Address::invalid();
  Address allocaAddr = Address::invalid();
  Address openMPLocalAddr = Address::invalid();
  if (getLangOpts().OpenMP && openMPLocalAddr.isValid()) {
    llvm_unreachable("NYI");
  } else if (Ty->isConstantSizeType()) {
    // If this value is an array or struct with a statically determinable
    // constant initializer, there are optimizations we can do.
    //
    // TODO: We should constant-evaluate the initializer of any variable,
    // as long as it is initialized by a constant expression. Currently,
    // isConstantInitializer produces wrong answers for structs with
    // reference or bitfield members, and a few other cases, and checking
    // for POD-ness protects us from some of these.
    if (D.getInit() && (Ty->isArrayType() || Ty->isRecordType()) &&
        (D.isConstexpr() ||
         ((Ty.isPODType(getContext()) ||
           getContext().getBaseElementType(Ty)->isObjCObjectPointerType()) &&
          D.getInit()->isConstantInitializer(getContext(), false)))) {

      // If the variable's a const type, and it's neither an NRVO
      // candidate nor a __block variable and has no mutable members,
      // emit it as a global instead.
      // Exception is if a variable is located in non-constant address space
      // in OpenCL.
      // TODO: deal with CGM.getCodeGenOpts().MergeAllConstants
      // TODO: perhaps we don't need this at all at CIR since this can
      // be done as part of lowering down to LLVM.
      if ((!getContext().getLangOpts().OpenCL ||
           Ty.getAddressSpace() == LangAS::opencl_constant) &&
          (!NRVO && !D.isEscapingByref() && CGM.isTypeConstant(Ty, true)))
        assert(0 && "not implemented");

      // Otherwise, tell the initialization code that we're in this case.
      emission.IsConstantAggregate = true;
    }

    // A normal fixed sized variable becomes an alloca in the entry block,
    // unless:
    // - it's an NRVO variable.
    // - we are compiling OpenMP and it's an OpenMP local variable.
    if (NRVO) {
      // The named return value optimization: allocate this variable in the
      // return slot, so that we can elide the copy when returning this
      // variable (C++0x [class.copy]p34).
      address = ReturnValue;
      allocaAddr = ReturnValue;

      if (const RecordType *RecordTy = Ty->getAs<RecordType>()) {
        const auto *RD = RecordTy->getDecl();
        const auto *CXXRD = dyn_cast<CXXRecordDecl>(RD);
        if ((CXXRD && !CXXRD->hasTrivialDestructor()) ||
            RD->isNonTrivialToPrimitiveDestroy()) {
          // In LLVM: Create a flag that is used to indicate when the NRVO was
          // applied to this variable. Set it to zero to indicate that NRVO was
          // not applied.
          llvm_unreachable("NYI");
        }
      }
    } else {
      if (isEscapingByRef)
        llvm_unreachable("NYI");

      mlir::Type allocaTy = getTypes().convertTypeForMem(Ty);
      CharUnits allocaAlignment = alignment;
      // Create the temp alloca and declare variable using it.
      mlir::Value addrVal;
      address = CreateTempAlloca(allocaTy, allocaAlignment,
                                 getLoc(D.getSourceRange()), D.getName(),
                                 /*ArraySize=*/nullptr, &allocaAddr);
      if (failed(declare(address, &D, Ty, getLoc(D.getSourceRange()), alignment,
                         addrVal))) {
        CGM.emitError("Cannot declare variable");
        return emission;
      }
      // TODO: what about emitting lifetime markers for MSVC catch parameters?
      // TODO: something like @llvm.lifetime.start/end here? revisit this later.
      assert(!UnimplementedFeature::shouldEmitLifetimeMarkers());
    }
  } else { // not openmp nor constant sized type
    llvm_unreachable("NYI");
  }

  emission.Addr = address;
  setAddrOfLocalVar(&D, emission.Addr);
  return emission;
}

/// Determine whether the given initializer is trivial in the sense
/// that it requires no code to be generated.
bool CIRGenFunction::isTrivialInitializer(const Expr *Init) {
  if (!Init)
    return true;

  if (const CXXConstructExpr *Construct = dyn_cast<CXXConstructExpr>(Init))
    if (CXXConstructorDecl *Constructor = Construct->getConstructor())
      if (Constructor->isTrivial() && Constructor->isDefaultConstructor() &&
          !Construct->requiresZeroInitialization())
        return true;

  return false;
}
void CIRGenFunction::buildAutoVarInit(const AutoVarEmission &emission) {
  assert(emission.Variable && "emission was not valid!");

  const VarDecl &D = *emission.Variable;
  QualType type = D.getType();

  // If this local has an initializer, emit it now.
  const Expr *Init = D.getInit();

  // TODO: in LLVM codegen if we are at an unreachable point, the initializer
  // isn't emitted unless it contains a label. What we want for CIR?
  assert(builder.getInsertionBlock());

  // Initialize the variable here if it doesn't have a initializer and it is a
  // C struct that is non-trivial to initialize or an array containing such a
  // struct.
  if (!Init && type.isNonTrivialToPrimitiveDefaultInitialize() ==
                   QualType::PDIK_Struct) {
    assert(0 && "not implemented");
    return;
  }

  const Address Loc = emission.Addr;
  // Check whether this is a byref variable that's potentially
  // captured and moved by its own initializer.  If so, we'll need to
  // emit the initializer first, then copy into the variable.
  assert(!UnimplementedFeature::capturedByInit() && "NYI");

  // Note: constexpr already initializes everything correctly.
  LangOptions::TrivialAutoVarInitKind trivialAutoVarInit =
      (D.isConstexpr()
           ? LangOptions::TrivialAutoVarInitKind::Uninitialized
           : (D.getAttr<UninitializedAttr>()
                  ? LangOptions::TrivialAutoVarInitKind::Uninitialized
                  : getContext().getLangOpts().getTrivialAutoVarInit()));

  auto initializeWhatIsTechnicallyUninitialized = [&](Address Loc) {
    if (trivialAutoVarInit ==
        LangOptions::TrivialAutoVarInitKind::Uninitialized)
      return;

    assert(0 && "unimplemented");
  };

  if (isTrivialInitializer(Init))
    return initializeWhatIsTechnicallyUninitialized(Loc);

  mlir::TypedAttr constant;
  if (emission.IsConstantAggregate ||
      D.mightBeUsableInConstantExpressions(getContext())) {
    // FIXME: Differently from LLVM we try not to emit / lower too much
    // here for CIR since we are interesting in seeing the ctor in some
    // analysis later on. So CIR's implementation of ConstantEmitter will
    // frequently return an empty Attribute, to signal we want to codegen
    // some trivial ctor calls and whatnots.
    constant = ConstantEmitter(*this).tryEmitAbstractForInitializer(D);
    if (constant && !constant.isa<mlir::cir::ZeroAttr>() &&
        (trivialAutoVarInit !=
         LangOptions::TrivialAutoVarInitKind::Uninitialized)) {
      llvm_unreachable("NYI");
    }
  }

  if (!constant) {
    initializeWhatIsTechnicallyUninitialized(Loc);
    LValue lv = LValue::makeAddr(Loc, type, AlignmentSource::Decl);
    buildExprAsInit(Init, &D, lv);
    // In case lv has uses it means we indeed initialized something
    // out of it while trying to build the expression, mark it as such.
    auto addr = lv.getAddress().getPointer();
    assert(addr && "Should have an address");
    auto allocaOp = dyn_cast_or_null<mlir::cir::AllocaOp>(addr.getDefiningOp());
    assert(allocaOp && "Address should come straight out of the alloca");

    if (!allocaOp.use_empty())
      allocaOp.setInitAttr(mlir::UnitAttr::get(builder.getContext()));
    return;
  }

  if (!emission.IsConstantAggregate) {
    llvm_unreachable("NYI");
  }

  llvm_unreachable("NYI");
}

void CIRGenFunction::buildAutoVarCleanups(const AutoVarEmission &emission) {
  assert(emission.Variable && "emission was not valid!");

  // TODO: in LLVM codegen if we are at an unreachable point codgen
  // is ignored. What we want for CIR?
  assert(builder.getInsertionBlock());
  const VarDecl &D = *emission.Variable;

  // Check the type for a cleanup.
  // TODO: something like emitAutoVarTypeCleanup
  if (QualType::DestructionKind dtorKind = D.needsDestruction(getContext()))
    assert(0 && "not implemented");

  // In GC mode, honor objc_precise_lifetime.
  if (getContext().getLangOpts().getGC() != LangOptions::NonGC &&
      D.hasAttr<ObjCPreciseLifetimeAttr>())
    assert(0 && "not implemented");

  // Handle the cleanup attribute.
  if (const CleanupAttr *CA = D.getAttr<CleanupAttr>())
    assert(0 && "not implemented");

  // TODO: handle block variable
}

/// Emit code and set up symbol table for a variable declaration with auto,
/// register, or no storage class specifier. These turn into simple stack
/// objects, globals depending on target.
void CIRGenFunction::buildAutoVarDecl(const VarDecl &D) {
  AutoVarEmission emission = buildAutoVarAlloca(D);
  buildAutoVarInit(emission);
  buildAutoVarCleanups(emission);
}

void CIRGenFunction::buildVarDecl(const VarDecl &D) {
  if (D.hasExternalStorage()) {
    assert(0 && "should we just returns is there something to track?");
    // Don't emit it now, allow it to be emitted lazily on its first use.
    return;
  }

  // Some function-scope variable does not have static storage but still
  // needs to be emitted like a static variable, e.g. a function-scope
  // variable in constant address space in OpenCL.
  if (D.getStorageDuration() != SD_Automatic)
    assert(0 && "not implemented");

  if (D.getType().getAddressSpace() == LangAS::opencl_local)
    assert(0 && "not implemented");

  assert(D.hasLocalStorage());

  CIRGenFunction::VarDeclContext varDeclCtx{*this, &D};
  return buildAutoVarDecl(D);
}

void CIRGenFunction::buildNullabilityCheck(LValue LHS, mlir::Value RHS,
                                           SourceLocation Loc) {
  if (!SanOpts.has(SanitizerKind::NullabilityAssign))
    return;

  llvm_unreachable("NYI");
}

void CIRGenFunction::buildScalarInit(const Expr *init, mlir::Location loc,
                                     LValue lvalue) {
  // TODO: this is where a lot of ObjC lifetime stuff would be done.
  mlir::Value value = buildScalarExpr(init);
  SourceLocRAIIObject Loc{*this, loc};
  buildStoreThroughLValue(RValue::get(value), lvalue);
  return;
}

void CIRGenFunction::buildExprAsInit(const Expr *init, const ValueDecl *D,
                                     LValue lvalue, bool capturedByInit) {
  SourceLocRAIIObject Loc{*this, getLoc(init->getSourceRange())};
  if (capturedByInit)
    llvm_unreachable("NYI");

  QualType type = D->getType();

  if (type->isReferenceType()) {
    RValue rvalue = buildReferenceBindingToExpr(init);
    if (capturedByInit)
      llvm_unreachable("NYI");
    buildStoreThroughLValue(rvalue, lvalue);
    return;
  }
  switch (CIRGenFunction::getEvaluationKind(type)) {
  case TEK_Scalar:
    buildScalarInit(init, getLoc(D->getSourceRange()), lvalue);
    return;
  case TEK_Complex: {
    assert(0 && "not implemented");
    return;
  }
  case TEK_Aggregate:
    assert(!type->isAtomicType() && "NYI");
    AggValueSlot::Overlap_t Overlap = AggValueSlot::MayOverlap;
    if (isa<VarDecl>(D))
      Overlap = AggValueSlot::DoesNotOverlap;
    else if (auto *FD = dyn_cast<FieldDecl>(D))
      assert(false && "Field decl NYI");
    else
      assert(false && "Only VarDecl implemented so far");
    // TODO: how can we delay here if D is captured by its initializer?
    buildAggExpr(init,
                 AggValueSlot::forLValue(lvalue, AggValueSlot::IsDestructed,
                                         AggValueSlot::DoesNotNeedGCBarriers,
                                         AggValueSlot::IsNotAliased, Overlap));
    return;
  }
  llvm_unreachable("bad evaluation kind");
}

void CIRGenFunction::buildDecl(const Decl &D) {
  switch (D.getKind()) {
  default:
    llvm_unreachable("Unknown decl kind");
  case Decl::BuiltinTemplate:
  case Decl::TranslationUnit:
  case Decl::ExternCContext:
  case Decl::Namespace:
  case Decl::UnresolvedUsingTypename:
  case Decl::ClassTemplateSpecialization:
  case Decl::ClassTemplatePartialSpecialization:
  case Decl::VarTemplateSpecialization:
  case Decl::VarTemplatePartialSpecialization:
  case Decl::TemplateTypeParm:
  case Decl::UnresolvedUsingValue:
  case Decl::NonTypeTemplateParm:
  case Decl::CXXDeductionGuide:
  case Decl::CXXMethod:
  case Decl::CXXConstructor:
  case Decl::CXXDestructor:
  case Decl::CXXConversion:
  case Decl::Field:
  case Decl::MSProperty:
  case Decl::IndirectField:
  case Decl::ObjCIvar:
  case Decl::ObjCAtDefsField:
  case Decl::ParmVar:
  case Decl::ImplicitParam:
  case Decl::ClassTemplate:
  case Decl::VarTemplate:
  case Decl::FunctionTemplate:
  case Decl::TypeAliasTemplate:
  case Decl::TemplateTemplateParm:
  case Decl::ObjCMethod:
  case Decl::ObjCCategory:
  case Decl::ObjCProtocol:
  case Decl::ObjCInterface:
  case Decl::ObjCCategoryImpl:
  case Decl::ObjCImplementation:
  case Decl::ObjCProperty:
  case Decl::ObjCCompatibleAlias:
  case Decl::PragmaComment:
  case Decl::PragmaDetectMismatch:
  case Decl::AccessSpec:
  case Decl::LinkageSpec:
  case Decl::Export:
  case Decl::ObjCPropertyImpl:
  case Decl::FileScopeAsm:
  case Decl::Friend:
  case Decl::FriendTemplate:
  case Decl::Block:
  case Decl::Captured:
  case Decl::ClassScopeFunctionSpecialization:
  case Decl::UsingShadow:
  case Decl::ConstructorUsingShadow:
  case Decl::ObjCTypeParam:
  case Decl::Binding:
  case Decl::UnresolvedUsingIfExists:
    llvm_unreachable("Declaration should not be in declstmts!");
  case Decl::Record:    // struct/union/class X;
  case Decl::CXXRecord: // struct/union/class X; [C++]
    assert(0 && "Not implemented");
    return;
  case Decl::Enum: // enum X;
    assert(0 && "Not implemented");
    return;
  case Decl::Function:     // void X();
  case Decl::EnumConstant: // enum ? { X = ? }
  case Decl::StaticAssert: // static_assert(X, ""); [C++0x]
  case Decl::Label:        // __label__ x;
  case Decl::Import:
  case Decl::MSGuid: // __declspec(uuid("..."))
  case Decl::TemplateParamObject:
  case Decl::OMPThreadPrivate:
  case Decl::OMPAllocate:
  case Decl::OMPCapturedExpr:
  case Decl::OMPRequires:
  case Decl::Empty:
  case Decl::Concept:
  case Decl::LifetimeExtendedTemporary:
  case Decl::RequiresExprBody:
  case Decl::UnnamedGlobalConstant:
    // None of these decls require codegen support.
    return;

  case Decl::NamespaceAlias:
    assert(0 && "Not implemented");
    return;
  case Decl::Using: // using X; [C++]
    assert(0 && "Not implemented");
    return;
  case Decl::UsingEnum: // using enum X; [C++]
    assert(0 && "Not implemented");
    return;
  case Decl::UsingPack:
    assert(0 && "Not implemented");
    return;
  case Decl::UsingDirective: // using namespace X; [C++]
    assert(0 && "Not implemented");
    return;
  case Decl::Var:
  case Decl::Decomposition: {
    const VarDecl &VD = cast<VarDecl>(D);
    assert(VD.isLocalVarDecl() &&
           "Should not see file-scope variables inside a function!");
    buildVarDecl(VD);
    if (auto *DD = dyn_cast<DecompositionDecl>(&VD))
      assert(0 && "Not implemented");

    // FIXME: add this
    // if (auto *DD = dyn_cast<DecompositionDecl>(&VD))
    //   for (auto *B : DD->bindings())
    //     if (auto *HD = B->getHoldingVar())
    //       EmitVarDecl(*HD);
    return;
  }

  case Decl::OMPDeclareReduction:
  case Decl::OMPDeclareMapper:
    assert(0 && "Not implemented");

  case Decl::Typedef:     // typedef int X;
  case Decl::TypeAlias: { // using X = int; [C++0x]
    assert(0 && "Not implemented");
  }
  }
}
