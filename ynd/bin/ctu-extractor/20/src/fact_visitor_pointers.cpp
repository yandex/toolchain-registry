// fact_visitor_pointers.cpp — FactVisitor's pointer-resolution and
// call-site-emission methods: what an expression's pointer value points to
// (Steensgaard-lite unification classes, ParamRegion, AllocationId, Field),
// and the CallSite fact (W3 call graph) for each call/construct/destructor.

#include "fact_visitor.h"

#include "entity_id.h"
#include "source_utils.h"

// Mirrors analyzer/libc_stubs.go's syntheticGlobalId exactly, so both sides
// agree on the same entity id for the same reserved name.
static uint64_t syntheticGlobalId(llvm::StringRef name) {
  return ctu::entityId(ctu::KIND_VAR, name, /*tuQualified=*/false, "");
}

// "__synthetic_errno_buf" — see analyzer/libc_stubs.go's synthErrnoBuf.
static constexpr const char *kSynthErrnoBuf = "__synthetic_errno_buf";

// Pointer arithmetic on an address formed from a subobject may leave that
// particular field/array element. Retain only the enclosing object/region;
// keeping a Field path here would incorrectly prove `&array[0] + 1` disjoint
// from array[1].
static void dropFieldPrecision(facts_pb::Aml *aml) {
  while (aml->has_field()) {
    facts_pb::Aml base = aml->field().base();
    *aml = std::move(base);
  }
}

// A pointer-valued glvalue loads a pointer from storage: `object.end`,
// `table[index]`, or a local pointer variable. A Field AML resolved from such
// an expression describes the loaded pointer's pointee abstraction, not the
// address of the field/element that stores the pointer. Pointer arithmetic
// must therefore retain that Field identity. In contrast, `&object.field`
// and an array-to-pointer decay are prvalues after stripping casts and still
// need dropFieldPrecision above.
static bool pointerValueLoadedFromStorage(Expr *expr) {
  expr = expr->IgnoreParenImpCasts();
  while (auto *cast = dyn_cast<ExplicitCastExpr>(expr))
    expr = cast->getSubExpr()->IgnoreParenImpCasts();
  return expr->isGLValue() && expr->getType()->isPointerType();
}

// True only when an object expression denotes storage rooted in a temporary
// whose lifetime is owned by the current full-expression. Besides a direct
// prvalue (`make().f()`), retain that proof through dot-access to a real
// subobject (`make().field.f()`). Do not follow arrows or reference-valued
// fields: those designate separately selected storage.
static bool isFreshTemporaryObjectStorage(Expr *expr) {
  if (!expr) return false;

  bool materialized = false;
  Expr *current = expr;
  while (current) {
    if (auto *EWC = dyn_cast<ExprWithCleanups>(current)) {
      current = EWC->getSubExpr();
      continue;
    }
    if (auto *MTE = dyn_cast<MaterializeTemporaryExpr>(current)) {
      materialized = true;
      current = MTE->getSubExpr();
      continue;
    }
    if (auto *BTE = dyn_cast<CXXBindTemporaryExpr>(current)) {
      materialized = true;
      current = BTE->getSubExpr();
      continue;
    }

    current = current->IgnoreParenImpCasts();
    if (auto *ME = dyn_cast<MemberExpr>(current)) {
      if (ME->isArrow()) return false;
      auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
      if (!FD || FD->getType()->isReferenceType()) return false;
      current = ME->getBase();
      continue;
    }
    break;
  }

  return materialized || (current && current->isPRValue());
}

uint32_t FactVisitor::getOrAssignUnifyClassId(const VarDecl *VD) {
  auto ins = unifyClassIds_.try_emplace(VD, nextUnifyClassId_);
  if (ins.second) ++nextUnifyClassId_;
  return ins.first->second;
}

uint32_t FactVisitor::getOrAssignCallResultClassId(const Expr *E) {
  auto ins = callResultClassIds_.try_emplace(E, nextUnifyClassId_);
  if (ins.second) ++nextUnifyClassId_;
  return ins.first->second;
}

uint32_t FactVisitor::getOrAssignAllocationId(const Expr *E) {
  auto ins = allocationIds_.try_emplace(E, allocIdCounter_);
  if (ins.second) ++allocIdCounter_;
  return ins.first->second;
}

uint32_t FactVisitor::getOrAssignReturnClassId(const facts_pb::EntityId &fnId) {
  auto ins = returnClassIds_.try_emplace(fnId.id(), nextUnifyClassId_);
  if (ins.second) ++nextUnifyClassId_;
  return ins.first->second;
}

bool FactVisitor::isPointerType(QualType QT) const {
  return QT->isPointerType() || QT->isReferenceType() ||
         QT->isFunctionPointerType() || QT->isMemberFunctionPointerType() ||
         QT->isMemberPointerType();
}

bool FactVisitor::isKnownAllocator(FunctionDecl *FD) {
  if (!FD) return false;
  // getName() asserts Name.isIdentifier() — operator new/delete,
  // conversion operators, and other special-function names are not
  // simple identifiers.  Check isIdentifier() first, then only call
  // getName() when it's safe.
  if (!FD->getDeclName().isIdentifier()) {
    OverloadedOperatorKind op = FD->getOverloadedOperator();
    return op == OO_New || op == OO_Array_New;
  }
  StringRef name = FD->getName();
  return name == "malloc" || name == "calloc" || name == "realloc" ||
         name == "mmap" || name == "mmap64";
}

bool FactVisitor::isErrnoLocation(FunctionDecl *FD) {
  if (!FD) return false;
  if (!FD->getDeclName().isIdentifier()) return false;
  return FD->getName() == "__errno_location";
}

bool FactVisitor::isStdFunctionLocal(const VarDecl *VD) {
  if (!VD || VD->hasGlobalStorage()) return false;
  const CXXRecordDecl *RD = VD->getType()->getAsCXXRecordDecl();
  if (!RD) return false;
  if (const ClassTemplateSpecializationDecl *Spec =
          dyn_cast<ClassTemplateSpecializationDecl>(RD))
    return Spec->getSpecializedTemplate()->getQualifiedNameAsString() ==
           "std::function";
  return RD->getQualifiedNameAsString() == "std::function";
}

void FactVisitor::handleStdFunctionAssignment(VarDecl *VD, Expr *RHS,
                                               const facts_pb::EntityId &fn) {
  if (!isStdFunctionLocal(VD) || !RHS) return;
  RHS = RHS->IgnoreParenImpCasts();

  // `std::function<S> f = target` and `f = target` both flow the callable
  // into f's synthetic class.  Constructors are intentionally unwrapped only
  // for their single source argument; allocator-arg and copy/move forms are
  // outside this small model and remain conservative.
  if (auto *CCE = dyn_cast<CXXConstructExpr>(RHS)) {
    if (CCE->getNumArgs() == 1)
      RHS = CCE->getArg(0);
    else
      RHS = nullptr;
  }

  facts_pb::Aml target;
  bool resolved = RHS && resolvePointerTargetAml(RHS, &target) &&
                  target.has_func_target();
  facts_pb::Fact f;
  facts_pb::UnifyConstraint *uc = f.mutable_unify_constraint();
  *uc->mutable_function() = fn;
  uc->mutable_a()->mutable_unify_class()->set_class_id(
      getOrAssignUnifyClassId(VD));
  if (resolved) {
    *uc->mutable_b() = std::move(target);
  } else {
    // The object may contain any callable compatible with its signature.
    // Keep this explicit so a partially-known target set is never mistaken
    // for exact at a later invocation.
    uc->mutable_b()->mutable_widening()->set_kind(
        facts_pb::Widening::W_UNKNOWN_EVERYTHING);
  }
  uc->set_reason(facts_pb::UnifyConstraint::R_FUNC_PTR_STORE);
  setLoc(uc->mutable_loc(), VD->getLocation(), SM);
  f.set_origin(originOf(isSystemLoc(VD->getLocation(), SM)));
  g_facts.push_back(std::move(f));
}

bool FactVisitor::resolvePointerTargetAml(Expr *E, facts_pb::Aml *aml) {
  // A non-capturing lambda converted to a raw function pointer is represented
  // as ExprWithCleanups -> conversion-operator call -> materialized
  // LambdaExpr.  The conversion's generated __invoke thunk is merely a
  // forwarding implementation detail; model the lambda's real call operator
  // instead, whose body carries the actual footprint.
  auto lambdaFromTemporary = [](Expr *X) -> LambdaExpr * {
    while (X) {
      X = X->IgnoreParenImpCasts();
      if (auto *EWC = dyn_cast<ExprWithCleanups>(X)) {
        X = EWC->getSubExpr();
        continue;
      }
      if (auto *MTE = dyn_cast<MaterializeTemporaryExpr>(X)) {
        X = MTE->getSubExpr();
        continue;
      }
      if (auto *BTE = dyn_cast<CXXBindTemporaryExpr>(X)) {
        X = BTE->getSubExpr();
        continue;
      }
      return dyn_cast<LambdaExpr>(X);
    }
    return nullptr;
  };

  if (auto *EWC = dyn_cast<ExprWithCleanups>(E))
    E = EWC->getSubExpr();
  E = E->IgnoreParenImpCasts();
  if (auto *DAE = dyn_cast<CXXDefaultArgExpr>(E))
    E = DAE->getExpr()->IgnoreParenImpCasts();
  // Strip explicit casts (e.g. (int*)malloc(...)) — they don't change
  // what the pointer points to, just the type through which it's viewed.
  while (auto *CE = dyn_cast<ExplicitCastExpr>(E))
    E = CE->getSubExpr()->IgnoreParenImpCasts();
  if (auto *BO = dyn_cast<BinaryOperator>(E);
      BO && (BO->getOpcode() == BO_PtrMemD ||
             BO->getOpcode() == BO_PtrMemI) &&
      BO->getRHS()->getType()->isMemberFunctionPointerType()) {
    // `(object.*method)` and `(pointer->*method)` bind a receiver but retain
    // the callable identity carried by the member-function pointer RHS.
    return resolvePointerTargetAml(BO->getRHS(), aml);
  }
  if (auto *MCE = dyn_cast<CXXMemberCallExpr>(E)) {
    if (auto *conversion = dyn_cast_or_null<CXXConversionDecl>(MCE->getDirectCallee())) {
      if (conversion->getConversionType()->isFunctionPointerType()) {
        if (LambdaExpr *LE = lambdaFromTemporary(MCE->getImplicitObjectArgument())) {
          computeFuncEntityId(LE->getCallOperator(), Ctx, SM,
                              aml->mutable_func_target()->mutable_function_id());
          return true;
        }
      }
    }
  }
  // std::function can hold a non-capturing lambda directly; unlike the raw
  // function-pointer conversion above there need not be a conversion call in
  // the initializer AST.  A captureless closure is safe to identify with its
  // call operator. Capturing lambdas retain object state and are deliberately
  // left to the conservative path for now.
  if (auto *LE = dyn_cast<LambdaExpr>(E)) {
    if (LE->capture_size() == 0) {
      computeFuncEntityId(LE->getCallOperator(), Ctx, SM,
                          aml->mutable_func_target()->mutable_function_id());
      return true;
    }
  }
  if (auto *BO = dyn_cast<BinaryOperator>(E);
      BO && E->getType()->isPointerType() &&
      (BO->getOpcode() == BO_Add || BO->getOpcode() == BO_Sub ||
       BO->getOpcode() == BO_AddAssign || BO->getOpcode() == BO_SubAssign)) {
    // p + n and p - n stay within p's enclosing allocation/object. For
    // n + p, only addition is valid, so use the pointer-typed RHS.
    Expr *base = BO->getLHS()->getType()->isPointerType() ? BO->getLHS() :
                 (BO->getOpcode() == BO_Add && BO->getRHS()->getType()->isPointerType()
                      ? BO->getRHS() : nullptr);
    if (base && resolvePointerTargetAml(base, aml)) {
      if (!pointerValueLoadedFromStorage(base))
        dropFieldPrecision(aml);
      return true;
    }
  }
  if (auto *UO = dyn_cast<UnaryOperator>(E);
      UO && E->getType()->isPointerType() && UO->isIncrementDecrementOp()) {
    if (resolvePointerTargetAml(UO->getSubExpr(), aml)) {
      if (!pointerValueLoadedFromStorage(UO->getSubExpr()))
        dropFieldPrecision(aml);
      return true;
    }
  }
  if (auto *BO = dyn_cast<BinaryOperator>(E);
      BO && BO->isAssignmentOp() && E->getType()->isPointerType()) {
    // The value of a pointer assignment expression is its RHS. This covers
    // chains such as `begin = p = parse(...);` without making the outer
    // assignment lose the return/target provenance already known for p.
    return resolvePointerTargetAml(BO->getRHS(), aml);
  }
  if (auto *BO = dyn_cast<BinaryOperator>(E);
      BO && BO->getOpcode() == BO_Comma && E->getType()->isPointerType()) {
    // The comma expression's value is its RHS; its LHS is still visited by
    // the expression walker for effects and sequencing.
    return resolvePointerTargetAml(BO->getRHS(), aml);
  }
  // A function name used as a pointer value (function-to-pointer decay):
  // `fp = someFunction;` or `fp = &someFunction;`.  The bare DeclRefExpr
  // case handles the implicit decay (`void (*fp)(int) = someFunction;`);
  // the UO_AddrOf case handles the explicit `&someFunction` form.  Both
  // produce a FuncTarget Aml so the function-pointer resolver can track
  // the set of concrete functions assigned to a pointer variable.
  auto tryFuncTarget = [&](Expr *Sub) -> bool {
    Sub = Sub->IgnoreParenImpCasts();
    if (auto *DRE = dyn_cast<DeclRefExpr>(Sub))
      if (auto *FD = dyn_cast<FunctionDecl>(DRE->getDecl())) {
        // Invoking a pointer to a virtual member still performs virtual
        // dispatch. A plain FuncTarget cannot encode that dispatch set, so
        // keep it unresolved rather than collapsing it to the base method.
        if (auto *MD = dyn_cast<CXXMethodDecl>(FD); MD && MD->isVirtual())
          return false;
        computeFuncEntityId(FD, Ctx, SM, aml->mutable_func_target()->mutable_function_id());
        return true;
      }
    return false;
  };
  if (auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_AddrOf) {
      if (tryFuncTarget(UO->getSubExpr())) return true;
      Expr *addressed = UO->getSubExpr()->IgnoreParenImpCasts();
      if (computeNamedVarAml(addressed, aml)) return true;
      // A call returning T&/T&& is an lvalue/xvalue designating the object
      // selected by the callee. Taking its address therefore preserves the
      // callee's existing return-class identity. This covers common library
      // APIs such as &container[index], &locale.category(), and &front().
      // Restrict the fallback to glvalue calls: taking the address of an
      // ordinary prvalue is ill-formed, and must never manufacture pointee
      // provenance from an unrelated call result.
      if (isa<CallExpr>(addressed) && addressed->isGLValue())
        return resolvePointerTargetAml(addressed, aml);
      return false;
    }
    // `(*callback)(args)` and `callback(args)` invoke the same function
    // pointer value.  Clang represents the explicit spelling with a
    // function-typed dereference, so forward to its pointer operand rather
    // than losing the NamedGlobal/Field/UnifyClass callee identity.
    if (UO->getOpcode() == UO_Deref && UO->getType()->isFunctionType())
      return resolvePointerTargetAml(UO->getSubExpr(), aml);
  } else if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (auto *FD = dyn_cast<FunctionDecl>(DRE->getDecl())) {
      computeFuncEntityId(FD, Ctx, SM, aml->mutable_func_target()->mutable_function_id());
      return true;
    }
    if (auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      // Clang lowers a range-for over an array through an implicit local
      // reference (`__range`) before deriving its begin/end pointers. A
      // local reference has one immutable binding, so forwarding directly
      // through that initializer is sound.
      if (VD->getType()->isReferenceType() && !isa<ParmVarDecl>(VD))
        return VD->hasInit() && resolvePointerTargetAml(VD->getInit(), aml);
      if (isTrackablePointerLocal(VD)) {
        aml->mutable_unify_class()->set_class_id(getOrAssignUnifyClassId(VD));
        return true;
      }
      if (isStdFunctionLocal(VD)) {
        aml->mutable_unify_class()->set_class_id(getOrAssignUnifyClassId(VD));
        return true;
      }
	  // A global pointer/function-pointer expression denotes stable storage
	  // shared across functions.  Preserve its NamedGlobal identity so an
	  // indirect call through it can consult concrete targets assigned to
	  // that particular slot, rather than falling back to every matching
	  // function signature in the whole target.
	  if (VD->hasGlobalStorage())
		return computeNamedVarAml(E, aml);
	  // An array lvalue decays to a pointer to its first element. Keep the
	  // whole named array/object region, not an element-specific Field path.
	  if (VD->getType()->isArrayType()) {
		return computeNamedVarAml(E, aml);
	  }
    }
  } else if (auto *CE = dyn_cast<CallExpr>(E)) {
    // A call to a known allocator: the returned pointer's pointee is a
    // fresh, per-call-site AllocationId — distinct from every other
    // allocation and from all concrete program locations.
    if (FunctionDecl *FD = CE->getDirectCallee()) {
      if (isKnownAllocator(FD)) {
        facts_pb::AllocationId *allocation = aml->mutable_allocation_id();
        allocation->set_id(getOrAssignAllocationId(CE));
        if (currentFunctionDecl_)
          computeFuncEntityId(currentFunctionDecl_, Ctx, SM,
                              allocation->mutable_function());
        return true;
      }
      // The errno accessor: its pointee is the synthetic NamedGlobal
      // "__synthetic_errno_buf", not a generic per-call-site UnifyClass.
      if (isErrnoLocation(FD)) {
        aml->mutable_named_global()->mutable_entity()->set_id(
            syntheticGlobalId(kSynthErrnoBuf));
        return true;
      }
      // A directly-consumed return value has no destination VarDecl to key
      // the existing cross-function machinery. Give each CallExpr its own
      // caller-local class and stamp that same class on the CallSite. Return
      // propagation then connects this particular result to the callee's
      // returned pointer/reference without merging separate calls to the
      // same forwarding helper.
      uint32_t returnClassId = getOrAssignCallResultClassId(CE);
      aml->mutable_unify_class()->set_class_id(returnClassId);
      pendingCallSiteReturnClassId_[CE] = returnClassId;
      return true;
    }
  } else if (auto *CCE = dyn_cast<CXXConstructExpr>(E)) {
    // operator new via placement-new syntax: CXXConstructExpr wraps the
    // new-expression; the actual allocation is the CXXNewExpr child.
    // Handled by the generic CallExpr path above if it's a direct call;
    // CXXConstructExpr itself is not an allocator.
  } else if (auto *NE = dyn_cast<CXXNewExpr>(E)) {
    // Non-placement operator new: the allocator is implicit (no
    // CallExpr), but the returned pointer is still a fresh allocation.
    facts_pb::AllocationId *allocation = aml->mutable_allocation_id();
    allocation->set_id(getOrAssignAllocationId(NE));
    if (currentFunctionDecl_)
      computeFuncEntityId(currentFunctionDecl_, Ctx, SM,
                          allocation->mutable_function());
    return true;
  } else if (auto *CO = dyn_cast<ConditionalOperator>(E)) {
    // Ternary ?: producing a pointer value.  Try both branches — if they
    // resolve to the SAME target, use it (both paths lead to the same
    // location).  Otherwise fall through to W_UNKNOWN_GLOBAL (sound:
    // the pointer could be either target, picking one would be unsound).
    // Also handles the common case where the ternary selects between
    // two string literals or two nullptr constants.
    // No BinaryConditionalOperator (GNU ?: with omitted middle) here —
    // it always evaluates to the condition's value, which is a bool,
    // never a pointer type in practice.
    facts_pb::Aml trueAml, falseAml;
    if (resolvePointerTargetAml(CO->getTrueExpr(), &trueAml) &&
        resolvePointerTargetAml(CO->getFalseExpr(), &falseAml) &&
        trueAml.SerializeAsString() == falseAml.SerializeAsString()) {
      *aml = trueAml;
      return true;
    }
  } else if (auto *ME = dyn_cast<MemberExpr>(E)) {
    // `expr.field`, `this->field`, or `trackedPointer->field`: a pointer
    // value loaded from a struct/class field. Keep the base symbolic when it
    // is a local pointer UnifyClass; canonicalAmls will later distribute the
    // field over every concrete pointee retained for that class. This covers
    // common aliases such as `T *tmp = (T *)arg->storage; tmp->buffer` without
    // claiming that the local pointer has a single target.
    auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
    if (!FD) return false;
    facts_pb::Aml baseAml;
    if (ME->isArrow()) {
      unsigned index, depth;
      if (auto *VD = trackablePointerVarOf(ME->getBase())) {
        baseAml.mutable_unify_class()->set_class_id(getOrAssignUnifyClassId(VD));
      } else if (paramRegionOf(ME->getBase(), index, depth)) {
        // `->` dereferences the base one more time than paramRegionOf has
        // accounted for.
        baseAml.mutable_param_region()->set_param_index(index);
        baseAml.mutable_param_region()->set_deref_depth(depth + 1);
      } else {
        // Preserve nested pointer fields such as `this->callbacks->read`.
        // The recursively resolved base remains symbolic (and therefore
        // widens normally when its pointee is unknown); appending a field
        // path does not select a concrete target prematurely.
        if (!resolvePointerTargetAml(ME->getBase(), &baseAml)) return false;
      }
    } else if (!computeNamedVarAml(ME->getBase()->IgnoreParenImpCasts(), &baseAml)) {
      unsigned idx, dep;
      if (paramRegionOf(ME->getBase(), idx, dep)) {
        baseAml.mutable_param_region()->set_param_index(idx);
        baseAml.mutable_param_region()->set_deref_depth(dep);
      } else if (!resolvePointerTargetAml(ME->getBase(), &baseAml)) {
        // A pointer-valued field can be selected from a reference-returning
        // direct call, e.g. `getData().pRoot`.  The call result has no named
        // caller-side storage, but its return class is still a stable base
        // for the existing field abstraction.  This is intentionally no
        // more precise than `object.pointerField`: it represents the field's
        // pointee region, rather than claiming an exact array element.
        return false;
      }
    }
    if (baseAml.has_field()) { *aml = baseAml; }
    else { *aml->mutable_field()->mutable_base() = baseAml; }
    aml->mutable_field()->add_field_path(FD->getFieldIndex());
    return true;
  } else if (isa<ArraySubscriptExpr>(E)) {
    // A pointer-valued array element denotes the element's storage, just as
    // a pointer-valued MemberExpr denotes its field storage above. Reuse the
    // named-variable resolver so constant indices remain field-sensitive and
    // runtime indices conservatively select the whole array region. This is
    // what lets a helper returning `callbacks[row][column]` carry the table's
    // concrete targets into its return class instead of falling back to every
    // address-taken function with the same signature.
    return computeNamedVarAml(E, aml);
  }
  unsigned index, depth;
  if (paramRegionOf(E, index, depth)) {
    aml->mutable_param_region()->set_param_index(index);
    aml->mutable_param_region()->set_deref_depth(depth + 1);
    return true;
  }
  if (isa<StringLiteral>(E) ||
      E->isNullPointerConstant(Ctx, Expr::NPC_ValueDependentIsNotNull) !=
          Expr::NPCK_NotNull) {
    aml->mutable_const_loc();
    return true;
  }
  return false;
}

void FactVisitor::handlePointerAssignment(VarDecl *VD, Expr *RHS, const facts_pb::EntityId &fn) {
  if (!RHS) return;
  RHS = RHS->IgnoreParenImpCasts();

  // Preserve each address-taken local that flows into a pointer local. This
  // is shared by the ordinary single-target path and the multi-target ternary
  // path below: omitting either branch would let a pointer passed onward look
  // non-escaping when it is not.
  auto recordAddressTaken = [&](const facts_pb::Aml &target, Expr *source) {
    const facts_pb::Aml *base = &target;
    while (base->has_field()) base = &base->field().base();
    if (!base->has_local()) return;
    Expr *addressed = source->IgnoreParenImpCasts();
    if (auto *UO = dyn_cast<UnaryOperator>(addressed);
        UO && UO->getOpcode() == UO_AddrOf)
      addressed = UO->getSubExpr()->IgnoreParenImpCasts();
    auto *rootVD = rootVarOfFieldChain(addressed);
    if (!rootVD || rootVD->hasGlobalStorage()) return;
    for (const auto &[otherLocal, otherPtr] : addrTakenTo_)
      if (otherPtr == VD && otherLocal != rootVD)
        addrTakenAmbiguousPtrs_.insert(VD);
    addrTakenTo_[rootVD] = VD;
  };

  // The analyzer can retain a bounded set of concrete pointer targets.  Do
  // not discard `cond ? a : b` merely because the branches differ: emit one
  // constraint per resolved branch into the same destination class.  This is
  // a sound union, unlike selecting either branch.  (Equal branches keep the
  // old single-constraint path below.)
  Expr *conditionalRHS = RHS;
  while (auto *EC = dyn_cast<ExplicitCastExpr>(conditionalRHS))
    conditionalRHS = EC->getSubExpr()->IgnoreParenImpCasts();
  if (auto *CO = dyn_cast<ConditionalOperator>(conditionalRHS);
      CO && CO->getType()->isPointerType()) {
    facts_pb::Aml trueAml, falseAml;
    if (resolvePointerTargetAml(CO->getTrueExpr(), &trueAml) &&
        resolvePointerTargetAml(CO->getFalseExpr(), &falseAml) &&
        trueAml.SerializeAsString() != falseAml.SerializeAsString()) {
      const uint32_t vdClassId = getOrAssignUnifyClassId(VD);
      auto emitTarget = [&](const facts_pb::Aml &target, Expr *branch) {
        recordAddressTaken(target, branch);
        emitReceiverTypeSeed(fn, target, branch);
        facts_pb::Fact f;
        facts_pb::UnifyConstraint *uc = f.mutable_unify_constraint();
        *uc->mutable_function() = fn;
        uc->mutable_a()->mutable_unify_class()->set_class_id(vdClassId);
        *uc->mutable_b() = target;
        uc->set_reason(facts_pb::UnifyConstraint::R_ASSIGN);
        setLoc(uc->mutable_loc(), RHS->getExprLoc(), SM);
        f.set_origin(originOf(isSystemLoc(RHS->getExprLoc(), SM)));
        g_facts.push_back(std::move(f));
      };
      emitTarget(trueAml, CO->getTrueExpr());
      emitTarget(falseAml, CO->getFalseExpr());
      // Preserve the exact destination class for each direct-call branch so
      // cross-function/stub return propagation can add all modeled targets.
      for (Expr *branch : {CO->getTrueExpr(), CO->getFalseExpr()}) {
        const Expr *candidate = branch->IgnoreParenImpCasts();
        while (auto *EC = dyn_cast<ExplicitCastExpr>(candidate))
          candidate = EC->getSubExpr()->IgnoreParenImpCasts();
        if (auto *CE = dyn_cast<CallExpr>(candidate)) {
          if (const FunctionDecl *FD = CE->getDirectCallee()) {
            if (!isKnownAllocator(const_cast<FunctionDecl *>(FD)))
              pendingCallSiteReturnClassId_[CE] = vdClassId;
          }
        }
      }
      return;
    }
  }

  facts_pb::Aml rhsAml;
  bool resolved = resolvePointerTargetAml(RHS, &rhsAml);
  std::string unresolvedPointerShape;
  std::string unresolvedPointerDetail;
  facts_pb::UnifyConstraint::Reason reason = facts_pb::UnifyConstraint::R_ASSIGN;
  if (auto *BO = dyn_cast<BinaryOperator>(RHS);
      BO && RHS->getType()->isPointerType() &&
      (BO->getOpcode() == BO_Add || BO->getOpcode() == BO_Sub ||
       BO->getOpcode() == BO_AddAssign || BO->getOpcode() == BO_SubAssign))
    reason = facts_pb::UnifyConstraint::R_POINTER_ARITH;
  if (auto *UO = dyn_cast<UnaryOperator>(RHS);
      UO && RHS->getType()->isPointerType() && UO->isIncrementDecrementOp())
    reason = facts_pb::UnifyConstraint::R_POINTER_ARITH;

  // Deferred escape tracking: if the RHS resolves to a Local Aml (i.e.,
  // &local or &local.field), record that this local's address was taken
  // and stored in VD (a trackable local pointer). At function end, the
  // transitive escape closure will decide whether the local actually
  // escapes (only if VD itself escapes). This avoids the over-
  // approximation where &local marks local as escaped even when the
  // address only flows to a non-escaping local pointer.
  if (resolved) {
    recordAddressTaken(rhsAml, RHS);
    emitReceiverTypeSeed(fn, rhsAml, RHS);
  }

  if (!resolved) {
    // If the RHS is a constant/literal (string literal, nullptr, numeric
    // constant, etc.), emit ConstLoc instead of Widening — the pointer
    // points to read-only memory, and writes through it are UB.  This
    // prevents the pointer's UnifyClass from being tainted to
    // W_UNKNOWN_EVERYTHING, which would falsely conflict with everything.
    // This also handles nullptr: even though "null doesn't point anywhere",
    // leaving the UnifyClass unresolved is worse — it would overlap with
    // everything.  Resolving to ConstLoc is sound (ConstLoc is disjoint
    // from everything, and writing through nullptr is UB anyway).
    RHS = RHS->IgnoreParenImpCasts();
    while (auto *ICE = dyn_cast<ImplicitCastExpr>(RHS))
      RHS = ICE->getSubExpr()->IgnoreParenImpCasts();
    while (auto *EC = dyn_cast<ExplicitCastExpr>(RHS))
      RHS = EC->getSubExpr()->IgnoreParenImpCasts();
    // R_CAST is a legacy enum name for any unresolved pointer-producing RHS.
    // Retain the post-cast AST class so analyzer telemetry can distinguish a
    // conditional, pointer-field value, unsupported builtin call, etc.
    unresolvedPointerShape = RHS->getStmtClassName();
    auto compactShape = [&](Expr *E) -> std::string {
      E = E->IgnoreParenImpCasts();
      while (auto *EC = dyn_cast<ExplicitCastExpr>(E))
        E = EC->getSubExpr()->IgnoreParenImpCasts();
      if (auto *CE = dyn_cast<CallExpr>(E)) {
        if (const FunctionDecl *FD = CE->getDirectCallee())
          return "CallExpr(" + FD->getNameAsString() + ")";
      }
      if (auto *BO = dyn_cast<BinaryOperator>(E))
        return "BinaryOperator(" + BO->getOpcodeStr().str() + ")";
      if (auto *UO = dyn_cast<UnaryOperator>(E))
        return "UnaryOperator(" + UnaryOperator::getOpcodeStr(UO->getOpcode()).str() + ")";
      return E->getStmtClassName();
    };
    if (auto *CO = dyn_cast<ConditionalOperator>(RHS)) {
      unresolvedPointerDetail = "true=" + compactShape(CO->getTrueExpr()) +
          " false=" + compactShape(CO->getFalseExpr());
    } else if (auto *BO = dyn_cast<BinaryOperator>(RHS)) {
      Expr *pointerBase = BO->getLHS()->getType()->isPointerType() ? BO->getLHS() :
          (BO->getRHS()->getType()->isPointerType() ? BO->getRHS() : nullptr);
      unresolvedPointerDetail = "op=" + BO->getOpcodeStr().str();
      if (pointerBase)
        unresolvedPointerDetail += " pointer-base=" + compactShape(pointerBase);
    } else if (auto *ME = dyn_cast<MemberExpr>(RHS)) {
      unresolvedPointerDetail = "base=" + compactShape(ME->getBase());
    } else if (auto *UO = dyn_cast<UnaryOperator>(RHS)) {
      unresolvedPointerDetail = "op=" + UnaryOperator::getOpcodeStr(UO->getOpcode()).str() +
          " operand=" + compactShape(UO->getSubExpr());
    }
    if (isa<StringLiteral>(RHS) ||
        isa<IntegerLiteral>(RHS) ||
        isa<FloatingLiteral>(RHS) ||
        isa<CXXBoolLiteralExpr>(RHS) ||
        isa<CharacterLiteral>(RHS) ||
        isa<CXXNullPtrLiteralExpr>(RHS) ||
        isa<GNUNullExpr>(RHS) ||
        RHS->isNullPointerConstant(Ctx, Expr::NPC_ValueDependentIsNotNull) !=
            Expr::NPCK_NotNull) {
      rhsAml.mutable_const_loc();
    } else if (isa<CallExpr>(RHS)) {
      // A non-allocator function call return value can never point to a
      // local variable in the caller — doing so would be a use-after-
      // return (undefined behavior).  Use W_UNKNOWN_GLOBAL (narrower,
      // sound) instead of W_UNKNOWN_EVERYTHING: it still overlaps with
      // NamedGlobal, ParamRegion, AllocationId, escaped Local, and other
      // Widening, but NOT with non-escaping Local or unresolved
      // UnifyClass — eliminating false positives from function-call-
      // sourced pointer taint while remaining provably sound.
      rhsAml.mutable_widening()->set_kind(facts_pb::Widening::W_UNKNOWN_GLOBAL);
    } else {
      // An unrecognized RHS (MemberExpr, ArraySubscriptExpr,
      // ConditionalOperator, cast, or any other expression that
      // produces a pointer value) can never point to a non-escaping
      // local variable in the current function — taking the address
      // of a local is always a concrete UO_AddrOf handled above,
      // and an opaque call's return already uses the CallExpr path
      // above.  Use W_UNKNOWN_GLOBAL (narrower, sound) instead of
      // W_UNKNOWN_EVERYTHING: it still overlaps with NamedGlobal,
      // escaped Local, ParamRegion, AllocationId, and other
      // Widening, but NOT with non-escaping Local or unresolved
      // UnifyClass — eliminating false positives from the
      // overwhelming majority of "unrecognized expression" cases.
      rhsAml.mutable_widening()->set_kind(facts_pb::Widening::W_UNKNOWN_GLOBAL);
    }
    reason = facts_pb::UnifyConstraint::R_CAST;
  }

  uint32_t vdClassId = getOrAssignUnifyClassId(VD);

  facts_pb::Fact f;
  facts_pb::UnifyConstraint *uc = f.mutable_unify_constraint();
  *uc->mutable_function() = fn;
  uc->mutable_a()->mutable_unify_class()->set_class_id(vdClassId);
  *uc->mutable_b() = rhsAml;
  uc->set_reason(reason);
  if (!unresolvedPointerShape.empty())
    uc->set_unresolved_pointer_shape(unresolvedPointerShape);
  if (!unresolvedPointerDetail.empty())
    uc->set_unresolved_pointer_detail(unresolvedPointerDetail);
  setLoc(uc->mutable_loc(), RHS->getExprLoc(), SM);
  f.set_origin(originOf(isSystemLoc(RHS->getExprLoc(), SM)));
  g_facts.push_back(std::move(f));

  // If the RHS is a (direct, non-allocator) call, remember that this
  // call's return value flows into VD's UnifyClass.  emitCallSite will
  // consume this on its way past the same CallExpr and stamp
  // CallSite.caller_return_class_id, so the analyzer's cross-function
  // return-value propagation can rewrite VD's class root to the
  // callee's resolved return target — no fragile position-based or
  // full-facts-scan matching needed.
  //
  // Look through any remaining explicit casts (e.g. `(int*)malloc(...)`
  // — but allocators are excluded below; a `(T*)f()` with a non-allocator
  // f still matches).  IgnoreParenImpCasts was already applied above; we
  // still need ExplicitCastExpr since C-style casts and static_cast are
  // NOT implicit.
  const Expr *CEcand = RHS;
  while (auto *EC = dyn_cast<ExplicitCastExpr>(CEcand))
    CEcand = EC->getSubExpr()->IgnoreParenImpCasts();
  if (auto *CE = dyn_cast<CallExpr>(CEcand)) {
    FunctionDecl *FD = const_cast<FunctionDecl *>(CE->getDirectCallee());
    if (!FD || !isKnownAllocator(FD))
      pendingCallSiteReturnClassId_[CE] = vdClassId;
  }
}

void FactVisitor::handleReceiverTypeStore(Expr *LHS, Expr *RHS,
                                          const facts_pb::EntityId &fn) {
  if (!LHS || !RHS || trackablePointerVarOf(LHS)) return;
  facts_pb::Aml destination;
  if (!computeNamedVarAml(LHS->IgnoreParenImpCasts(), &destination)) return;
  emitReceiverTypeStore(destination, LHS->getType(), RHS, fn);
}

void FactVisitor::emitReceiverTypeStore(const facts_pb::Aml &destination,
                                        QualType storedType, Expr *RHS,
                                        const facts_pb::EntityId &fn) {
  if (!RHS) return;
  if (!storedType->isPointerType() && !storedType->isReferenceType()) return;
  QualType objectType = storedType->isPointerType()
      ? storedType->getPointeeType() : storedType.getNonReferenceType();
  CXXRecordDecl *RD = objectType->getAsCXXRecordDecl();
  RD = RD ? RD->getDefinition() : nullptr;
  if (!RD || !RD->isPolymorphic()) return;

  // Preserve a bounded union for conditional stores just as
  // handlePointerAssignment does for pointer locals. Collapsing
  // `field = cond ? &a : &b` to unknown loses both exact dynamic classes
  // and makes every constructed sibling override look reachable later.
  Expr *conditionalRHS = RHS->IgnoreParenImpCasts();
  while (auto *EC = dyn_cast<ExplicitCastExpr>(conditionalRHS))
    conditionalRHS = EC->getSubExpr()->IgnoreParenImpCasts();
  if (auto *CO = dyn_cast<ConditionalOperator>(conditionalRHS);
      CO && CO->getType()->isPointerType()) {
    emitReceiverTypeStore(destination, storedType, CO->getTrueExpr(), fn);
    emitReceiverTypeStore(destination, storedType, CO->getFalseExpr(), fn);
    return;
  }
  if (RHS->isNullPointerConstant(Ctx, Expr::NPC_ValueDependentIsNotNull) !=
      Expr::NPCK_NotNull)
    return;

  facts_pb::Aml source;
  bool resolved = computeReceiverAml(RHS, true, &source);
  if (resolved) emitReceiverTypeSeed(fn, source, RHS);

  facts_pb::Fact fact;
  facts_pb::UnifyConstraint *uc = fact.mutable_unify_constraint();
  *uc->mutable_function() = fn;
  *uc->mutable_a() = std::move(destination);
  if (resolved) {
    *uc->mutable_b() = std::move(source);
  } else {
    uc->mutable_b()->mutable_widening()->set_kind(
        facts_pb::Widening::W_UNKNOWN_EVERYTHING);
  }
  uc->set_reason(facts_pb::UnifyConstraint::R_RECEIVER_TYPE_STORE);
  setLoc(uc->mutable_loc(), RHS->getExprLoc(), SM);
  fact.set_origin(originOf(isSystemLoc(RHS->getExprLoc(), SM)));
  g_facts.push_back(std::move(fact));
}

bool FactVisitor::computeReceiverAml(Expr *E, bool isArrow, facts_pb::Aml *aml) {
  if (!E) return false;
  Expr *source = E;
  for (;;) {
    source = source->IgnoreParenImpCasts();
    if (auto *EWC = dyn_cast<ExprWithCleanups>(source)) {
      source = EWC->getSubExpr();
      continue;
    }
    if (auto *MTE = dyn_cast<MaterializeTemporaryExpr>(source)) {
      source = MTE->getSubExpr();
      continue;
    }
    if (auto *BTE = dyn_cast<CXXBindTemporaryExpr>(source)) {
      source = BTE->getSubExpr();
      continue;
    }
    if (auto *cast = dyn_cast<ExplicitCastExpr>(source)) {
      source = cast->getSubExpr();
      continue;
    }
    break;
  }

  // A directly-consumed call result has no VarDecl to key the existing
  // unification machinery. Give this CallExpr a synthetic caller-local
  // class and stamp the same class on the inner CallSite. Return propagation
  // then connects the callee's returned object/pointee to this outer receiver.
  if (auto *call = dyn_cast<CallExpr>(source)) {
    uint32_t id = getOrAssignCallResultClassId(call);
    aml->mutable_unify_class()->set_class_id(id);
    pendingCallSiteReturnClassId_[call] = id;
    return true;
  }

  E = E->IgnoreParenImpCasts();
  if (isArrow) return resolvePointerTargetAml(E, aml);
  return computeNamedVarAml(E, aml);
}

void FactVisitor::emitDynamicTypeUse(Expr *object, const facts_pb::EntityId &fn,
                                     facts_pb::DynamicTypeUse::Kind kind,
                                     SourceLocation loc) {
  if (!object) return;
  object = object->IgnoreParenImpCasts();
  // A cast changes the static view used to perform the operation, not the
  // identity of the object whose dynamic type is consulted. In particular,
  // Rule 15.1.1 explicitly covers access to the current object through a
  // pointer or reference to a child class.
  while (auto *cast = dyn_cast<ExplicitCastExpr>(object))
    object = cast->getSubExpr()->IgnoreParenImpCasts();
  facts_pb::Fact fact;
  facts_pb::DynamicTypeUse *use = fact.mutable_dynamic_type_use();
  *use->mutable_function() = fn;
  use->set_kind(kind);
  bool resolved = false;
  if (object->getType()->isPointerType()) {
    resolved = resolvePointerTargetAml(object, use->mutable_object());
  } else {
    resolved = computeNamedVarAml(object, use->mutable_object());
  }
  use->set_has_complete_object_identity(resolved);
  if (!resolved)
    use->mutable_object()->mutable_widening()->set_kind(
        facts_pb::Widening::W_UNKNOWN_EVERYTHING);
  setLoc(use->mutable_loc(), loc, SM);
  fact.set_origin(originOf(isSystemLoc(loc, SM)));
  g_facts.push_back(std::move(fact));
}

void FactVisitor::emitCallSite(FunctionDecl *calleeFD, bool indirect,
                   const std::vector<Expr *> &args, const facts_pb::EntityId &fn,
                   uint64_t feid, uint64_t nodeId, SourceLocation loc,
                   CXXMethodDecl *virtualMD,
                   Expr *receiverExpr, bool receiverIsArrow,
                   CXXMethodDecl *methodForReceiver,
                   const facts_pb::Aml *rawReceiver,
                   const Expr *sourceCall,
                   Expr *calleeExpr) {
  facts_pb::Fact f;
  facts_pb::CallSite *cs = f.mutable_call_site();
  *cs->mutable_caller() = fn;
  if (calleeFD && !indirect)
    computeFuncEntityId(calleeFD, Ctx, SM, cs->mutable_callee());
  cs->set_full_expr_id(feid);
  cs->set_node_id(nodeId);
  if (sourceCall)
    callSiteNodes_[sourceCall] = std::make_pair(feid, nodeId);

  // If handlePointerAssignment recorded that this call's return flows
  // into a tracked pointer local's UnifyClass, stamp the id here so
  // the analyzer can rewrite exactly that class's root to the callee's
  // resolved return target — see cross_unify.go's crossFunctionUnify.
  if (sourceCall) {
    auto it = pendingCallSiteReturnClassId_.find(sourceCall);
    if (it != pendingCallSiteReturnClassId_.end()) {
      cs->set_caller_return_class_id(it->second);
      cs->set_caller_return_class_id_set(true);
      pendingCallSiteReturnClassId_.erase(it);
    }
  }

  // Resolve real args first into a local vector so the receiver (below)
  // can be INSERTED at exactly the reserved slot (methodForReceiver's own
  // param count) regardless of how many real args there are — args.size()
  // usually already equals it (Clang materializes default args), but a
  // variadic callee's extra variadic-only args (positions beyond
  // getNumParams(), never associated with any ParmVarDecl and so already
  // unreachable via paramRegionOf inside the callee regardless of
  // position) would otherwise collide with the reserved slot if just
  // appended at the end instead of inserted.
  std::vector<facts_pb::Aml> resolvedArgs;
  std::vector<bool> argMayReachCallerStorage;
  resolvedArgs.reserve(args.size() + 1);
  argMayReachCallerStorage.reserve(args.size() + 1);
  FunctionDecl *parameterOwner = calleeFD ? calleeFD : methodForReceiver;
  for (size_t argIndex = 0; argIndex < args.size(); ++argIndex) {
    Expr *arg = args[argIndex];
    QualType parameterType = arg->getType();
    if (parameterOwner && argIndex < parameterOwner->getNumParams())
      parameterType = parameterOwner->getParamDecl(argIndex)->getType();
    // A copied scalar/enum cannot give the callee access to its source
    // object. Records stay conservative because they can contain pointers.
    bool mayReachCallerStorage = parameterType->isPointerType() ||
        parameterType->isReferenceType() || parameterType->isRecordType() ||
        parameterType->isArrayType() || parameterType->isMemberPointerType();
    // Passing a local pointer variable exposes the pointer's value to the
    // callee. If that pointer was initialized from &local, the deferred
    // addrTakenTo_ closure in VisitFunctionDecl will consequently retain
    // local's escaped bit. Without this, p = &local; take_ptr(p) was
    // incorrectly treated as though p never escaped.
    if (auto *pointerLocal = trackablePointerVarOf(arg))
      markEscapedLocal(pointerLocal, feid);
    facts_pb::Aml a;
    // A prvalue bound to a reference parameter denotes fresh temporary
    // storage, not an arbitrary global. The old generic fallback mapped
    // computed temporaries (especially call results) to W_UNKNOWN_GLOBAL;
    // remapping a callee's precise ParamRegion effects through that value
    // then manufactured conflicts with unrelated globals. Keep it as an
    // escaped Local: it is provably distinct from every named object, while
    // still conservatively overlapping opaque/widened effects because the
    // callee receives its address and could publish it during this call.
    bool freshReferenceTemporary = parameterOwner &&
        argIndex < parameterOwner->getNumParams() &&
        parameterOwner->getParamDecl(argIndex)->getType()->isReferenceType() &&
        !isPointerType(arg->getType()) &&
        isFreshTemporaryObjectStorage(arg);
    if (freshReferenceTemporary) {
      facts_pb::Local *local = a.mutable_local();
      local->set_local_id(nextTempLocalId_++);
      local->set_escaped(true);
    } else if (!resolvePointerTargetAml(arg, &a)) {
      // Try computeNamedVarAml before falling back to Widening — this
      // handles named variables (globals, locals, fields) passed as
      // arguments, e.g. `strchr(buf, 'c')` where buf is a local array
      // or global.  Without this, the argument resolves to
      // W_UNKNOWN_GLOBAL and return-value propagation can't remap the
      // callee's return target through it.
      Expr *namedArg = arg->IgnoreParenImpCasts();
      if (computeNamedVarAml(namedArg, &a)) {
        // success — a is now a real NamedGlobal/Local/Field Aml
      } else {
        // If the argument is a constant/literal expression (string literal,
        // nullptr, numeric constant, etc.), emit ConstLoc instead of
        // Widening — constants are in read-only memory and never conflict
        // with program variables.  resolvePointerTargetAml already returns
        // false for nullptr (see handlePointerAssignment's null check), and
        // string literals / other literals don't match any of its cases.
        arg = arg->IgnoreParenImpCasts();
        // Strip implicit casts (e.g. string literal -> const char*)
        while (auto *ICE = dyn_cast<ImplicitCastExpr>(arg))
          arg = ICE->getSubExpr()->IgnoreParenImpCasts();
        if (isa<StringLiteral>(arg) ||
            isa<IntegerLiteral>(arg) ||
            isa<FloatingLiteral>(arg) ||
            isa<CXXBoolLiteralExpr>(arg) ||
            isa<CharacterLiteral>(arg) ||
            isa<CXXNullPtrLiteralExpr>(arg) ||
            isa<GNUNullExpr>(arg) ||
            arg->isNullPointerConstant(Ctx, Expr::NPC_ValueDependentIsNotNull) !=
                Expr::NPCK_NotNull) {
          a.mutable_const_loc();
        } else {
          // An unrecognized argument expression (MemberExpr, ArraySubscriptExpr,
          // cast, or any other expression producing a pointer value) can never
          // point to a non-escaping local of the caller — taking an address of
          // a local is always UO_AddrOf handled above.  Use W_UNKNOWN_GLOBAL
          // (narrower, sound) instead of W_UNKNOWN_EVERYTHING.
          a.mutable_widening()->set_kind(facts_pb::Widening::W_UNKNOWN_GLOBAL);
        }
      }
    }
    emitReceiverTypeSeed(fn, a, arg);
    resolvedArgs.push_back(std::move(a));
    argMayReachCallerStorage.push_back(mayReachCallerStorage);
  }
  if ((methodForReceiver || rawReceiver) && (receiverExpr || rawReceiver)) {
    // An indirect pointer-to-member call has no MethodDecl at the call site,
    // but its receiver still follows all explicit method arguments.
    size_t slot = methodForReceiver
        ? static_cast<size_t>(methodForReceiver->getNumParams())
        : resolvedArgs.size();
    cs->set_receiver_arg_index(static_cast<uint32_t>(slot));
    cs->set_receiver_arg_index_set(true);
    while (resolvedArgs.size() < slot) {
      facts_pb::Aml pad;
      pad.mutable_widening()->set_kind(facts_pb::Widening::W_UNKNOWN_GLOBAL);
      resolvedArgs.push_back(std::move(pad));
    }
    facts_pb::Aml recv;
    facts_pb::Aml typeFlowRecv;
    if (rawReceiver) {
      recv = *rawReceiver;
      typeFlowRecv = recv;
    } else if (!computeReceiverAml(receiverExpr, receiverIsArrow, &typeFlowRecv)) {
      // An unrecognized receiver expression can never be a non-escaping
      // local of the caller — taking the address of a local is always
      // UO_AddrOf.  Use W_UNKNOWN_GLOBAL (narrower, sound).
      typeFlowRecv.mutable_widening()->set_kind(
          facts_pb::Widening::W_UNKNOWN_GLOBAL);
    }
    recv = typeFlowRecv;
    // A dot-call on a prvalue operates on fresh temporary storage. Keep that
    // concrete identity in args so the callee's `this` effects cannot alias
    // unrelated globals. Arrow calls instead operate on the pointee selected
    // by the pointer expression, so a prvalue pointer is not fresh storage.
    if (receiverExpr && !receiverIsArrow &&
        isFreshTemporaryObjectStorage(receiverExpr)) {
      facts_pb::Local *local = recv.mutable_local();
      local->set_local_id(nextTempLocalId_++);
      local->set_escaped(true);
      *cs->mutable_receiver_type_flow_arg() = typeFlowRecv;
    }
    if (receiverExpr)
      emitReceiverTypeSeed(fn, typeFlowRecv, receiverExpr);
    if (virtualMD)
      *cs->mutable_virtual_receiver() = typeFlowRecv;
    resolvedArgs.insert(resolvedArgs.begin() + slot, std::move(recv));
    argMayReachCallerStorage.insert(argMayReachCallerStorage.begin() + slot,
                                    true);
  }
  for (facts_pb::Aml &a : resolvedArgs) *cs->add_args() = std::move(a);
  for (bool mayReach : argMayReachCallerStorage)
    cs->add_arg_may_reach_caller_storage(mayReach);
  cs->set_has_complete_arg_storage_reachability(true);

  cs->set_is_indirect(indirect);
  // For an indirect (non-virtual) call, resolve the callee expression's
  // own pointer value so the analyzer's function-pointer resolver can ask
  // "what does THIS call's callee pointer resolve to."  Virtual calls are
  // excluded: their resolution path is CHA (cha.go), not the function-
  // pointer resolver.  A destructor's callee is implicit (no expression),
  // so calleeExpr is null there — that's fine, it's virtual anyway.
  if (indirect && !virtualMD && calleeExpr) {
    facts_pb::Aml calleeAml;
    if (resolvePointerTargetAml(calleeExpr, &calleeAml))
      *cs->mutable_indirect_callee() = std::move(calleeAml);

    // Stamp the callee expression's static type — the function type
    // (stripping the outer pointer if present) as a string — so Phase 2
    // of the function-pointer resolver can look up candidates by
    // signature when Phase 1's intraprocedural resolution fails.  The
    // key format is the same as what typeToString returns for a function
    // type, e.g. "void (int *)" — the analyzer's addr_taken.go signature
    // index reconstructs the same string from FuncDecl.ret_type + params.
    {
      QualType calleeType = calleeExpr->getType();
      Expr *strippedCallee = calleeExpr->IgnoreParenImpCasts();
      if (auto *BO = dyn_cast<BinaryOperator>(strippedCallee);
          BO && (BO->getOpcode() == BO_PtrMemD ||
                 BO->getOpcode() == BO_PtrMemI) &&
          BO->getRHS()->getType()->isMemberFunctionPointerType()) {
        calleeType = BO->getRHS()->getType();
      }
      cs->set_has_complete_indirect_callee_member_identity(true);
      cs->set_indirect_callee_is_member_function(
          calleeType->isMemberFunctionPointerType());
      if (auto *ptrTy = calleeType->getAs<clang::PointerType>())
        calleeType = ptrTy->getPointeeType();
      if (auto *memberPtrTy = calleeType->getAs<clang::MemberPointerType>())
        calleeType = memberPtrTy->getPointeeType();

      if (!calleeType.isNull() && calleeType->isFunctionType()) {
        uint64_t typeHashLo = 0, typeHashHi = 0;
        typeFingerprint(calleeType, Ctx, &typeHashLo, &typeHashHi);
        cs->set_indirect_callee_type_hash(typeHashLo);
        cs->set_indirect_callee_type_hash_hi(typeHashHi);
        cs->set_has_complete_indirect_callee_type_identity(true);
      }

      // FuncDecl signatures are stored as return type plus explicit parameter
      // types. Build the call-site key the same way instead of printing the
      // whole function type, which also includes qualifiers such as noexcept
      // or a member function's trailing const and therefore cannot match the
      // declaration-side index.
      if (auto *funcTy = calleeType->getAs<clang::FunctionProtoType>()) {
        std::string signature = typeToString(funcTy->getReturnType(), Ctx);
        signature += " (";
        for (unsigned i = 0; i < funcTy->getNumParams(); ++i) {
          if (i != 0) signature += ", ";
          signature += typeToString(funcTy->getParamType(i), Ctx);
        }
        signature += ")";
        cs->set_indirect_callee_type(std::move(signature));
      } else {
        cs->set_indirect_callee_type(typeToString(calleeType, Ctx));
      }
    }
  }
  if (virtualMD) {
    cs->set_is_virtual(true);
    cs->set_virtual_method_name(virtualMD->getNameAsString());
    cs->set_virtual_method_mangled(mangleName(virtualMD, Ctx));
    // Use the call expression's actual static receiver type, not the method's
    // declaring class. An inherited Base::f invoked through Derived& can only
    // dispatch within Derived's descendant subtree; starting CHA at Base
    // incorrectly admits every sibling override.
    CXXRecordDecl *receiverRD = nullptr;
    if (receiverExpr) {
      QualType receiverType = receiverExpr->IgnoreParenImpCasts()->getType();
      if (receiverType->isPointerType())
        receiverType = receiverType->getPointeeType();
      if (receiverType->isReferenceType())
        receiverType = receiverType.getNonReferenceType();
      receiverRD = receiverType->getAsCXXRecordDecl();
    }
    if (!receiverRD) receiverRD = virtualMD->getParent();
    if (receiverRD)
      cs->set_receiver_class_mangled(
          mangleType(Ctx.getRecordType(receiverRD), Ctx));
  }
  setLoc(cs->mutable_loc(), loc, SM);
  f.set_origin(originOf(isSystemLoc(loc, SM)));
  g_facts.push_back(std::move(f));
}

bool FactVisitor::computeNamedVarAml(Expr *E, facts_pb::Aml *aml) {
  if (auto *ME = dyn_cast<MemberExpr>(E)) {
    auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
    if (!FD) return false;  // e.g. a static data member: base isn't addressing
    facts_pb::Aml baseAml;
    if (ME->isArrow()) {
      auto *VD = trackablePointerVarOf(ME->getBase());
      unsigned index, depth;
      if (VD) {
        baseAml.mutable_unify_class()->set_class_id(getOrAssignUnifyClassId(VD));
      } else if (paramRegionOf(ME->getBase(), index, depth)) {
        // `->` reads through the base one more time than paramRegionOf's
        // own depth accounts for (it was computed on the base expression,
        // not on a deref of it).
        baseAml.mutable_param_region()->set_param_index(index);
        baseAml.mutable_param_region()->set_deref_depth(depth + 1);
      } else if (!resolvePointerTargetAml(ME->getBase(), &baseAml)) {
        return false;
      }
    } else if (!computeNamedVarAml(ME->getBase()->IgnoreParenImpCasts(), &baseAml)) {
      return false;
    }
    if (baseAml.has_field()) {
      *aml = baseAml;
    } else {
      *aml->mutable_field()->mutable_base() = baseAml;
    }
    aml->mutable_field()->add_field_path(FD->getFieldIndex());
    return true;
  }
  if (auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() != UO_Deref) return false;
    auto *VD = trackablePointerVarOf(UO->getSubExpr());
    if (VD) {
      aml->mutable_unify_class()->set_class_id(getOrAssignUnifyClassId(VD));
      return true;
    }
    unsigned index, depth;
    if (paramRegionOf(E, index, depth)) {
      aml->mutable_param_region()->set_param_index(index);
      aml->mutable_param_region()->set_deref_depth(depth);
      return true;
    }
    // *malloc(n), *new T, etc.: the pointee of an allocator call's
    // returned pointer — resolve via the same path as pointer assignment
    // RHS resolution (reuses resolvePointerTargetAml).
    return resolvePointerTargetAml(UO->getSubExpr(), aml);
  }
  if (auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
    // Region being subscripted: pointee of a tracked pointer/param, or the
    // named array object itself (e.g. `arr[i]`, `s.arr[i]`).
    facts_pb::Aml regionAml;
    auto *VD = trackablePointerVarOf(ASE->getBase());
    unsigned index, depth;
    if (VD) {
      regionAml.mutable_unify_class()->set_class_id(getOrAssignUnifyClassId(VD));
    } else if (paramRegionOf(ASE->getBase(), index, depth)) {
      regionAml.mutable_param_region()->set_param_index(index);
      regionAml.mutable_param_region()->set_deref_depth(depth + 1);
    } else if (!computeNamedVarAml(ASE->getBase()->IgnoreParenImpCasts(), &regionAml)) {
      // Not a named variable/field — try allocator call (malloc(n)[i],
      // new T[i][j], etc.).
      if (!resolvePointerTargetAml(ASE->getBase(), &regionAml))
        return false;
    }
    // Constant index -> its own Field path element (disjoint from other
    // constants, like struct fields). Non-constant -> bare whole-region.
    Expr::EvalResult evalResult;
    Expr *idxExpr = ASE->getIdx();
    if (!idxExpr->isValueDependent() &&
        idxExpr->EvaluateAsInt(evalResult, Ctx) && evalResult.Val.isInt()) {
      if (regionAml.has_field()) {
        *aml = regionAml;
      } else {
        *aml->mutable_field()->mutable_base() = regionAml;
      }
      aml->mutable_field()->add_field_path(
          static_cast<uint32_t>(evalResult.Val.getInt().getExtValue()));
    } else {
      *aml = regionAml;
    }
    return true;
  }
  auto *DRE = dyn_cast<DeclRefExpr>(E);
  if (!DRE) return false;
  auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
  if (!VD) return false;
  if (VD->getType()->isReferenceType()) {
    auto *PVD = dyn_cast<ParmVarDecl>(VD);
    if (PVD) {
      aml->mutable_param_region()->set_param_index(PVD->getFunctionScopeIndex());
      aml->mutable_param_region()->set_deref_depth(1);
      return true;
    }
    // A local reference has one immutable binding. Following its initializer
    // is therefore sound and is required for compiler-generated range-for
    // variables (`auto&& __range = array; __begin = __range;`).
    return VD->hasInit() && resolvePointerTargetAml(VD->getInit(), aml);
  }
  if (VD->hasGlobalStorage()) {
    computeVarEntityId(VD, Ctx, SM, aml->mutable_named_global()->mutable_entity());
  } else {
    auto ins = localIds_.try_emplace(VD, static_cast<uint32_t>(localIds_.size()));
    aml->mutable_local()->set_local_id(ins.first->second);
  }
  return true;
}
// Return a record only when the expression itself proves the complete
// object's dynamic type. Merely casting a Base* to Derived* is not evidence:
// explicit casts are peeled and the underlying origin must still be exact.
CXXRecordDecl *FactVisitor::exactDynamicClass(Expr *E) const {
  if (!E) return nullptr;
  // getAsCXXRecordDecl() also returns declarations for incomplete record
  // types.  CXXRecordDecl::isPolymorphic(), however, reads definition data
  // and is not valid until a definition is available.  This is common for
  // generated-code sentinels such as
  //
  //   struct DefaultTypeInternal;
  //   extern DefaultTypeInternal default_instance;
  //
  // whose address is passed through otherwise ordinary call expressions.
  // An incomplete declaration proves no dynamic type, so keep the receiver
  // unknown instead of querying it as though it were a definition.
  auto polymorphicDefinition = [](QualType T) -> CXXRecordDecl * {
    if (T.isNull()) return nullptr;
    CXXRecordDecl *RD = T->getAsCXXRecordDecl();
    if (!RD) return nullptr;
    RD = RD->getDefinition();
    return RD && RD->isPolymorphic() ? RD : nullptr;
  };
  auto finalPolymorphicPointee = [&](QualType T) -> CXXRecordDecl * {
    if (T.isNull() || (!T->isPointerType() && !T->isReferenceType()))
      return nullptr;
    CXXRecordDecl *RD = polymorphicDefinition(T->getPointeeType());
    return RD && RD->hasAttr<FinalAttr>() ? RD : nullptr;
  };
  if (auto *EWC = dyn_cast<ExprWithCleanups>(E))
    E = EWC->getSubExpr();
  E = E->IgnoreParenImpCasts();
  while (auto *EC = dyn_cast<ExplicitCastExpr>(E))
    E = EC->getSubExpr()->IgnoreParenImpCasts();
  // A pointer/reference normally carries only a static lower bound on the
  // dynamic type. For a final class that lower bound is exact: no derived
  // dynamic type can exist. This gives receiver flow a sound seed for final
  // objects entering through parameters, fields, and factory returns instead
  // of leaving those sites permanently at bottom. Explicit casts were peeled
  // above, so a cast from Base* to Final* is deliberately not accepted as
  // evidence about the underlying object.
  if (CXXRecordDecl *RD = finalPolymorphicPointee(E->getType()))
    return RD;
  if (auto *MTE = dyn_cast<MaterializeTemporaryExpr>(E))
    return exactDynamicClass(MTE->getSubExpr());
  if (auto *BTE = dyn_cast<CXXBindTemporaryExpr>(E))
    return exactDynamicClass(BTE->getSubExpr());
  if (auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_AddrOf)
      return exactDynamicClass(UO->getSubExpr());
    return nullptr;
  }
  if (auto *NE = dyn_cast<CXXNewExpr>(E)) {
    QualType T = NE->getAllocatedType();
    return T.isNull() ? nullptr
                      : polymorphicDefinition(Ctx.getBaseElementType(T));
  }
  if (auto *CCE = dyn_cast<CXXConstructExpr>(E))
    return polymorphicDefinition(CCE->getType());
  if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
    if (!VD || VD->getType()->isPointerType() ||
        VD->getType()->isReferenceType())
      return nullptr;
    QualType T = Ctx.getBaseElementType(VD->getType());
    return polymorphicDefinition(T);
  }
  if (auto *CE = dyn_cast<CallExpr>(E)) {
    QualType result = CE->getCallReturnType(Ctx);
    if (!result->isPointerType() && !result->isReferenceType())
      return polymorphicDefinition(Ctx.getBaseElementType(result));
  }
  if (auto *ME = dyn_cast<MemberExpr>(E)) {
    auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
    if (!FD || FD->getType()->isPointerType() ||
        FD->getType()->isReferenceType())
      return nullptr;
    QualType T = Ctx.getBaseElementType(FD->getType());
    return polymorphicDefinition(T);
  }
  return nullptr;
}

void FactVisitor::emitReceiverTypeSeed(const facts_pb::EntityId &fn,
                                       const facts_pb::Aml &object, Expr *E) {
  CXXRecordDecl *RD = exactDynamicClass(E);
  if (!RD || object.has_widening() || object.has_const_loc() ||
      object.has_func_target())
    return;
  facts_pb::Fact fact;
  facts_pb::ReceiverTypeSeed *seed = fact.mutable_receiver_type_seed();
  *seed->mutable_function() = fn;
  *seed->mutable_object() = object;
  seed->set_class_mangled(mangleType(Ctx.getRecordType(RD), Ctx));
  setLoc(seed->mutable_loc(), E->getExprLoc(), SM);
  fact.set_origin(originOf(isSystemLoc(E->getExprLoc(), SM)));
  g_facts.push_back(std::move(fact));
}
