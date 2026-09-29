// fact_visitor_exprs.cpp — FactVisitor's full-expression walk: builds the
// ExprNode tree for each full-expression and emits Access facts for volatile
// and named-variable accesses (current-schema spine smoke).

#include "fact_visitor.h"

#include <functional>

#include "clang/AST/ExprConcepts.h"

#include "source_utils.h"

namespace {
bool isCurrentThisReceiver(Expr *E) {
  if (!E) return false;
  E = E->IgnoreParenImpCasts();
  if (isa<CXXThisExpr>(E)) return true;
  if (auto *UO = dyn_cast<UnaryOperator>(E))
    if (UO->getOpcode() == UO_Deref)
      return isa<CXXThisExpr>(UO->getSubExpr()->IgnoreParenImpCasts());
  return false;
}
}  // namespace

CXXMethodDecl *FactVisitor::devirtualizedMethod(CXXMethodDecl *method,
                                                Expr *receiver) const {
  if (!method || !receiver) return nullptr;
  if (auto *devirtualized = method->getDevirtualizedMethod(
          receiver, Ctx.getLangOpts().AppleKext))
    return devirtualized;

  auto *currentMethod = dyn_cast_or_null<CXXMethodDecl>(currentFunctionDecl_);
  bool duringLifetimeBoundary = isa_and_nonnull<CXXConstructorDecl>(currentMethod) ||
                                isa_and_nonnull<CXXDestructorDecl>(currentMethod);
  if (!duringLifetimeBoundary || !isCurrentThisReceiver(receiver))
    return nullptr;

  // In a Base constructor/destructor running as part of a Derived object's
  // lifetime, virtual dispatch is restricted to Base. If the current class
  // overrides the selected slot, getCorrespondingMethodInClass returns that
  // override; otherwise it returns the inherited implementation.
  return method->getCorrespondingMethodInClass(currentMethod->getParent());
}

void FactVisitor::emitObjectConstruction(CXXConstructExpr *E,
                                         const facts_pb::EntityId *fn) {
  if (!E || E->getConstructionKind() != CXXConstructionKind::Complete)
    return;
  CXXConstructorDecl *Ctor = E->getConstructor();
  CXXRecordDecl *RD = Ctor ? Ctor->getParent() : nullptr;
  if (!RD) return;

  // A non-polymorphic containing object can still create polymorphic member
  // complete objects, including via an implicit defaulted constructor whose
  // member-initializer expressions are not materialized at the call site.
  // Walk bases for their fields but never mark a base subobject itself as a
  // dynamic type. Union members are over-approximated deliberately.
  llvm::DenseSet<const CXXRecordDecl *> visiting;
  std::function<void(CXXRecordDecl *, bool)> emitRecord =
      [&](CXXRecordDecl *Record, bool isCompleteDynamicObject) {
        if (!Record) return;
        Record = Record->getDefinition();
        if (!Record) return;
        const CXXRecordDecl *canonical = Record->getCanonicalDecl();
        if (!visiting.insert(canonical).second) return;

        if (isCompleteDynamicObject && Record->isPolymorphic()) {
          facts_pb::Fact fact;
          facts_pb::ObjectConstruction *construction =
              fact.mutable_object_construction();
          if (fn) *construction->mutable_function() = *fn;
          construction->set_class_mangled(
              mangleType(Ctx.getRecordType(Record), Ctx));
          setLoc(construction->mutable_loc(), E->getExprLoc(), SM);
          fact.set_origin(originOf(isSystemLoc(E->getExprLoc(), SM)));
          g_facts.push_back(std::move(fact));
        }

        for (const CXXBaseSpecifier &Base : Record->bases())
          emitRecord(Base.getType()->getAsCXXRecordDecl(),
                     /*isCompleteDynamicObject=*/false);
        for (const FieldDecl *Field : Record->fields()) {
          QualType memberType = Ctx.getBaseElementType(Field->getType());
          emitRecord(memberType->getAsCXXRecordDecl(),
                     /*isCompleteDynamicObject=*/true);
        }
        visiting.erase(canonical);
      };
  emitRecord(RD, /*isCompleteDynamicObject=*/true);
}

void FactVisitor::emitFullExprs(Stmt *S, const facts_pb::EntityId &fn) {
  if (!S) return;
  if (auto *DS = dyn_cast<DeclStmt>(S)) {
    // `T *p = <expr>;` is a pointer assignment too, but it's not visible
    // as a BinaryOperator — the VarDecl's own initializer is the only
    // place it appears.
    for (Decl *D : DS->decls())
      if (auto *VD = dyn_cast<VarDecl>(D))
        if (isTrackablePointerLocal(VD) && VD->hasInit())
          handlePointerAssignment(VD, VD->getInit(), fn);
        else if (isStdFunctionLocal(VD) && VD->hasInit())
          handleStdFunctionAssignment(VD, VD->getInit(), fn);
    // fall through: the initializer's own value-access facts are still
    // emitted normally below via the generic recursion.
  }
  if (auto *RS = dyn_cast<ReturnStmt>(S)) {
    // A return statement that returns a pointer expression: emit a
    // UnifyConstraint with R_RETURN connecting the return expression
    // to a synthetic "retval" UnifyClass for this function.  The
    // cross-function unification pass uses this to resolve the
    // caller's pointer at call sites where the return value is used.
    if (Expr *RV = RS->getRetValue()) {
      // Check pointer-ness on the ORIGINAL expression, before stripping
      // ImplicitCastExpr — the array-to-pointer decay (ArrayToPointerDecay)
      // sits ON the ImplicitCastExpr, so stripping it turns the type
      // back into an array (e.g. `return buf_;` where buf_ is char[32]).
      // The value being returned is a pointer, regardless.
      bool returnsPointer = isPointerType(RV->getType());
      RV = RV->IgnoreParenImpCasts();
      if (returnsPointer) {
        facts_pb::Aml retAml;
        if (resolvePointerTargetAml(RV, &retAml)) {
          emitReceiverTypeSeed(fn, retAml, RV);
          facts_pb::Fact f;
          facts_pb::UnifyConstraint *uc = f.mutable_unify_constraint();
          *uc->mutable_function() = fn;
          uc->mutable_a()->mutable_unify_class()->set_class_id(getOrAssignReturnClassId(fn));
          *uc->mutable_b() = retAml;
          uc->set_reason(facts_pb::UnifyConstraint::R_RETURN);
          setLoc(uc->mutable_loc(), RV->getExprLoc(), SM);
          f.set_origin(originOf(isSystemLoc(RV->getExprLoc(), SM)));
          g_facts.push_back(std::move(f));
        }
      }
    }
    // Fall through: the return value expression is still emitted as a
    // full-expression below (for its own access facts).
  }
  if (auto *E = dyn_cast<Expr>(S)) {
    uint64_t feid = ++fullExprCounter_;
    buildNode(E, fn, feid, /*writeCtx=*/false);
    return;  // don't descend further; sub-exprs are handled by buildNode
  }
  for (Stmt *child : S->children()) emitFullExprs(child, fn);
}

bool FactVisitor::hasLValueToRValueConversion(Expr *E) {
  for (;;) {
    E = E->IgnoreParens();
    auto *ICE = dyn_cast<ImplicitCastExpr>(E);
    if (!ICE) return false;
    if (ICE->getCastKind() == CK_LValueToRValue) return true;
    E = ICE->getSubExpr();
  }
}

uint64_t FactVisitor::buildNode(Expr *E, const facts_pb::EntityId &fn, uint64_t feid,
                   bool writeCtx, bool readModifyWrite,
                   bool discarded) {
  bool isRead = hasLValueToRValueConversion(E);
  E = E->IgnoreParenImpCasts();

  // Transparent wrappers: no ExprNode of their own, just forward to the
  // real underlying expression (like parens/implicit casts above).
  if (auto *DAE = dyn_cast<CXXDefaultArgExpr>(E))
    // CXXDefaultArgExpr::children() is deliberately EMPTY (the same
    // default-arg expression is shared across every call site) — using
    // getExpr() instead is required, not just tidier, or a side effect in
    // a default argument is silently invisible at every call site.
    return buildNode(DAE->getExpr(), fn, feid, writeCtx, readModifyWrite, discarded);
  if (auto *DIE = dyn_cast<CXXDefaultInitExpr>(E))
    // Same issue, same fix, for default member initializers.
    return buildNode(DIE->getExpr(), fn, feid, writeCtx, readModifyWrite, discarded);
  if (auto *RBO = dyn_cast<CXXRewrittenBinaryOperator>(E))
    // Rewritten comparison (C++20 `<=>`-derived `<`/`==`/etc.): its
    // children() already reaches the real semantic form via the generic
    // fallback, so nothing was actually missed here — this just avoids an
    // extra, wrongly-labeled CK_LEAF-with-an-operand wrapper node.
    return buildNode(RBO->getSemanticForm(), fn, feid, writeCtx, readModifyWrite, discarded);
  // A NON-volatile glvalue cast to void (`(void)x;`) is a pure discard —
  // no lvalue-to-rvalue conversion, no reference bound, no address taken.
  // Recurse into the discarded subexpression (it may still contain real
  // side effects, e.g. `(void)f();` must still emit f()'s CallSite) but
  // mark it so the "neither read nor written" fallback further down
  // doesn't mistake the discard for reference-binding/escape. A volatile
  // operand is unaffected: the standard still requires its load, so
  // hasLValueToRValueConversion already returns true for it and isRead
  // takes precedence over this flag entirely.
  if (isa<ExplicitCastExpr>(E) && E->getType()->isVoidType()) {
    auto *CE = cast<CastExpr>(E);
    return buildNode(CE->getSubExpr(), fn, feid, /*writeCtx=*/false,
                      /*readModifyWrite=*/false, /*discarded=*/true);
  }

  uint64_t id = ++nodeCounter_;

  facts_pb::ConstructKind ck = facts_pb::CK_LEAF;
  std::vector<Kid> kids;
  if (isa<CXXNoexceptExpr>(E) || isa<UnaryExprOrTypeTraitExpr>(E) ||
      isa<RequiresExpr>(E)) {
    // noexcept(expr), sizeof(expr)/alignof, requires{...}: operand is
    // NEVER evaluated (T0.7) — no descent, so no facts from dead code.
    // (alignof never has an expr operand at all — always alignof(type).)
  } else if (auto *TIE = dyn_cast<CXXTypeidExpr>(E)) {
    // typeid(expr) is the one exception: evaluated at runtime iff the
    // operand is a glvalue of polymorphic type (needed for the vtable
    // lookup) — Clang already implements exactly this rule.
    if (!TIE->isTypeOperand() && TIE->isPotentiallyEvaluated()) {
      emitDynamicTypeUse(TIE->getExprOperand(), fn,
                         facts_pb::DynamicTypeUse::DTU_TYPEID,
                         TIE->getExprLoc());
      kids.push_back({TIE->getExprOperand(), false});
    }
  } else if (auto *DCE = dyn_cast<CXXDynamicCastExpr>(E)) {
    emitDynamicTypeUse(DCE->getSubExpr(), fn,
                       facts_pb::DynamicTypeUse::DTU_DYNAMIC_CAST,
                       DCE->getExprLoc());
    kids.push_back({DCE->getSubExpr(), false});
  } else if (auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->isAssignmentOp()) ck = facts_pb::CK_ASSIGN;
    else if (BO->getOpcode() == BO_LAnd) ck = facts_pb::CK_LOGICAL_AND;
    else if (BO->getOpcode() == BO_LOr) ck = facts_pb::CK_LOGICAL_OR;
    else if (BO->getOpcode() == BO_Comma) ck = facts_pb::CK_COMMA;
    else if (BO->getOpcode() == BO_Shl || BO->getOpcode() == BO_Shr)
      ck = facts_pb::CK_SHIFT;
    else if (BO->getOpcode() == BO_PtrMemD || BO->getOpcode() == BO_PtrMemI)
      ck = facts_pb::CK_PTR_TO_MEMBER;
    else ck = facts_pb::CK_BINOP;
    kids.push_back({BO->getLHS(), BO->isAssignmentOp(),
                    BO->isCompoundAssignmentOp()});
    kids.push_back({BO->getRHS(), false});
    if (BO->getOpcode() == BO_Assign) {  // plain assign only; `p += n`
                                          // doesn't change what class p's
                                          // pointee belongs to
      if (auto *DRE = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParenImpCasts()))
        if (auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
          if (isTrackablePointerLocal(VD))
            handlePointerAssignment(VD, BO->getRHS(), fn);
          else if (isStdFunctionLocal(VD))
            handleStdFunctionAssignment(VD, BO->getRHS(), fn);
      // Function-pointer targets need their own flow relation even when
      // stored in a global or a record field.  Phase 1 previously only saw
      // local UnifyClasses, and Phase 2 then fell back to every address-taken
      // function of the matching signature.  Here the A side is the actual
      // storage Aml (NamedGlobal/Field/ParamRegion), so a later indirect call
      // can use only functions assigned to that particular slot.
      if (BO->getLHS()->getType()->isFunctionPointerType() &&
          !trackablePointerVarOf(BO->getLHS())) {
        facts_pb::Aml lhsAml, rhsAml;
        if (computeNamedVarAml(BO->getLHS(), &lhsAml)) {
          bool resolved = resolvePointerTargetAml(BO->getRHS(), &rhsAml);
          // A null assignment supplies no callable target. It must not
          // invalidate targets supplied by other control-flow paths; a call
          // through null is UB and is already outside this rule's model.
          bool isNull = !resolved && BO->getRHS()->isNullPointerConstant(
              Ctx, Expr::NPC_ValueDependentIsNotNull) != Expr::NPCK_NotNull;
          if (!isNull) {
            facts_pb::Fact f;
            facts_pb::UnifyConstraint *uc = f.mutable_unify_constraint();
            *uc->mutable_function() = fn;
            *uc->mutable_a() = std::move(lhsAml);
            if (resolved) {
              *uc->mutable_b() = std::move(rhsAml);
            } else {
              // An unrecognised function-pointer value may name any function.
              // Taint this slot so the resolver refuses any partial target set.
              uc->mutable_b()->mutable_widening()->set_kind(
                  facts_pb::Widening::W_UNKNOWN_EVERYTHING);
            }
            uc->set_reason(facts_pb::UnifyConstraint::R_FUNC_PTR_STORE);
            setLoc(uc->mutable_loc(), BO->getExprLoc(), SM);
            f.set_origin(originOf(isSystemLoc(BO->getExprLoc(), SM)));
            g_facts.push_back(std::move(f));
          }
        }
      }
    }
    if ((BO->getOpcode() == BO_AddAssign || BO->getOpcode() == BO_SubAssign) &&
        BO->getType()->isPointerType())
      if (VarDecl *VD = trackablePointerVarOf(BO->getLHS()))
        handlePointerAssignment(VD, BO, fn);
  } else if (auto *CO = dyn_cast<ConditionalOperator>(E)) {
    ck = facts_pb::CK_TERNARY;
    kids.push_back({CO->getCond(), false});
    kids.push_back({CO->getTrueExpr(), writeCtx});
    kids.push_back({CO->getFalseExpr(), writeCtx});
  } else if (auto *UO = dyn_cast<UnaryOperator>(E)) {
    ck = facts_pb::CK_BINOP;  // treated as a value-computation node
    bool w = UO->isIncrementDecrementOp();
    kids.push_back({UO->getSubExpr(), w, w});  // ++/-- is read-modify-write
    // Pointer ++/-- changes the pointer's offset just like p = p + 1.
    // Record that relation so later dereferences retain the base region but
    // do not incorrectly retain an exact field/array-index path.
    if (w && E->getType()->isPointerType())
      if (VarDecl *VD = trackablePointerVarOf(UO->getSubExpr()))
        handlePointerAssignment(VD, UO, fn);
    if (UO->getOpcode() == UO_AddrOf) markEscapeIfLocal(UO->getSubExpr());
  } else if (auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
    ck = facts_pb::CK_SUBSCRIPT;
    kids.push_back({ASE->getLHS(), false});
    kids.push_back({ASE->getRHS(), false});
  } else if (auto *ME = dyn_cast<MemberExpr>(E)) {
    ck = facts_pb::CK_LEAF;
    if (ME->isArrow()) {
      // `->` reads the pointer's own value (needed to dereference) —
      // always visit the base separately, even though the pointee itself
      // may ALSO be classifiable now (Field/UnifyClass, computed again
      // below): the base's own read (e.g. catching `(p = &y) + p->field`,
      // reassigning p unsequenced with dereferencing it) is a distinct
      // event from whatever p points to.
      kids.push_back({ME->getBase(), false});
    } else {
      facts_pb::Aml discard;
      if (!computeNamedVarAml(E, &discard))
        kids.push_back({ME->getBase(), false});
      // else: a plain dot-chain, fully captured by this node's own Field/
      // NamedGlobal/Local Aml (recomputed below in the access-emission
      // block). Do NOT also descend into the base as a separate child —
      // `.` never converts its base via lvalue-to-rvalue (unlike `->`,
      // which reads the pointer), so the base would hit the "neither
      // read nor write" case and be wrongly flagged as escaping merely
      // for being the target of its own field access.
    }
  } else if (auto *OCE = dyn_cast<CXXOperatorCallExpr>(E)) {
    // Operator notation: follows the sequencing of the built-in operator it
    // overloads (rule 15), NOT ordinary call rules — an overloaded `<<`/`=`
    // is sequenced, not indeterminate. Clang can't distinguish operator
    // notation from explicit call syntax (`a.operator+(b)`) in the AST; we
    // assume operator notation, the overwhelmingly common case.
    unsigned n = OCE->getNumArgs();
    switch (OCE->getOperator()) {
      case OO_Equal:
        ck = facts_pb::CK_ASSIGN;
        kids.push_back({OCE->getArg(0), true, false});
        kids.push_back({OCE->getArg(1), false});
        break;
      case OO_PlusEqual: case OO_MinusEqual: case OO_StarEqual:
      case OO_SlashEqual: case OO_PercentEqual: case OO_CaretEqual:
      case OO_AmpEqual: case OO_PipeEqual: case OO_LessLessEqual:
      case OO_GreaterGreaterEqual:
        ck = facts_pb::CK_ASSIGN;
        kids.push_back({OCE->getArg(0), true, true});  // compound: RMW
        kids.push_back({OCE->getArg(1), false});
        break;
      case OO_Subscript:
        ck = facts_pb::CK_SUBSCRIPT;
        kids.push_back({OCE->getArg(0), false});
        kids.push_back({OCE->getArg(1), false});
        break;
      case OO_LessLess: case OO_GreaterGreater:
        ck = facts_pb::CK_SHIFT;
        kids.push_back({OCE->getArg(0), false});
        kids.push_back({OCE->getArg(1), false});
        break;
      case OO_ArrowStar:
        ck = facts_pb::CK_PTR_TO_MEMBER;
        kids.push_back({OCE->getArg(0), false});
        kids.push_back({OCE->getArg(1), false});
        break;
      case OO_AmpAmp:
        ck = facts_pb::CK_LOGICAL_AND;
        kids.push_back({OCE->getArg(0), false});
        kids.push_back({OCE->getArg(1), false});
        break;
      case OO_PipePipe:
        ck = facts_pb::CK_LOGICAL_OR;
        kids.push_back({OCE->getArg(0), false});
        kids.push_back({OCE->getArg(1), false});
        break;
      case OO_Comma:
        ck = facts_pb::CK_COMMA;
        kids.push_back({OCE->getArg(0), false});
        kids.push_back({OCE->getArg(1), false});
        break;
      case OO_PlusPlus: case OO_MinusMinus:
        ck = facts_pb::CK_BINOP;
        kids.push_back({OCE->getArg(0), true, true});  // RMW, prefix or postfix
        break;
      case OO_Call: {
        ck = facts_pb::CK_CALL;
        kids.push_back({OCE->getArg(0), false});  // the callable object
        for (unsigned i = 1; i < n; ++i) kids.push_back({OCE->getArg(i), false});
        FunctionDecl *calleeFD = OCE->getDirectCallee();
        CXXMethodDecl *virtualMD = nullptr;
        CXXMethodDecl *methodForReceiver = nullptr;
        if (calleeFD)
          if (auto *MD = dyn_cast<CXXMethodDecl>(calleeFD)) {
            methodForReceiver = MD;  // operator() is always a non-static member
            if (MD->isVirtual()) {
              emitDynamicTypeUse(OCE->getArg(0), fn,
                                 facts_pb::DynamicTypeUse::DTU_VIRTUAL_CALL,
                                 E->getExprLoc());
              if (auto *Devirt = devirtualizedMethod(MD, OCE->getArg(0))) {
                calleeFD = Devirt;
                methodForReceiver = Devirt;
              } else {
                virtualMD = MD;
              }
            }
          }
        bool indirect = !calleeFD || virtualMD;
        // std::function::operator() is a direct call to the type-erased
        // wrapper, but semantically it is an indirect call through the
        // object. For locals whose construction/assignment was recorded,
        // route it through the ordinary function-pointer resolver instead of
        // analyzing std::function's erased dispatch internals.
        bool stdFunctionCall = false;
        if (auto *MD = dyn_cast_or_null<CXXMethodDecl>(calleeFD))
          if (MD->getOverloadedOperator() == OO_Call) {
            if (auto *DRE = dyn_cast<DeclRefExpr>(
                    OCE->getArg(0)->IgnoreParenImpCasts()))
              if (auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
                stdFunctionCall = isStdFunctionLocal(VD);
          }
        if (stdFunctionCall) indirect = true;
        std::vector<Expr *> callArgs(OCE->arg_begin() + 1, OCE->arg_end());
        emitCallSite(indirect ? nullptr : calleeFD, indirect, callArgs, fn,
                     feid, id, E->getExprLoc(), virtualMD,
                     stdFunctionCall ? nullptr : OCE->getArg(0), /*receiverIsArrow=*/false,
                     stdFunctionCall ? nullptr : methodForReceiver,
                     /*rawReceiver=*/nullptr, /*sourceCall=*/OCE,
                     /*calleeExpr=*/(indirect && !virtualMD) ? OCE->getArg(0) : nullptr);
        break;
      }
      default:
        // Relational/bitwise/arithmetic/unary-not etc.: ordinary operator
        // rule (unsequenced), same as CK_BINOP.
        ck = facts_pb::CK_BINOP;
        for (unsigned i = 0; i < n; ++i) kids.push_back({OCE->getArg(i), false});
        break;
    }
  } else if (auto *CCE = dyn_cast<CXXConstructExpr>(E)) {
    emitObjectConstruction(CCE, &fn);
    ck = facts_pb::CK_CONSTRUCT_CALL;  // direct-init: args indeterminately sequenced
    for (Expr *a : CCE->arguments()) kids.push_back({a, false});
    if (CXXConstructorDecl *Ctor = CCE->getConstructor()) {
      // `va_list ap;` is represented on some targets as a CXXConstructExpr
      // for Clang's compiler-synthesized __va_list_tag constructor.  That
      // constructor has no source declaration and consequently no FuncDecl
      // fact.  It is initialization of automatic ABI state, not a callable
      // program function, so emitting it as a CallSite creates a spurious
      // direct unresolved callee at the declaration's source line.
      //
      // Compare the canonical types via ASTContext instead of matching the
      // target-specific __va_list_tag spelling: the builtin va_list layout
      // varies by target.
      if (!Ctx.hasSameType(CCE->getType(), Ctx.getBuiltinVaListType())) {
        std::vector<Expr *> callArgs(CCE->arg_begin(), CCE->arg_end());
        // Synthesize a fresh Local for the constructed object — it's
        // separate from any caller-observable Aml (same approach as
        // CXXBindTemporaryExpr below).  Without this, every constructor's
        // `this->member` writes widen the whole caller footprint through
        // collapseIfTainted.
        facts_pb::Aml receiver;
        receiver.mutable_local()->set_local_id(nextTempLocalId_++);
        emitCallSite(Ctor, /*indirect=*/false, callArgs, fn, feid, id,
                     E->getExprLoc(), /*virtualMD=*/nullptr,
                     /*receiverExpr=*/nullptr, /*receiverIsArrow=*/false,
                     Ctor, &receiver, /*sourceCall=*/CCE);
      }
    }
  } else if (auto *BTE = dyn_cast<CXXBindTemporaryExpr>(E)) {
    // A temporary requiring destruction at the end of the full-expression.
    // The destructor call itself has no AST representation of its own (no
    // operand) — but BTE->getTemporary()->getDestructor() gives the real
    // CXXDestructorDecl, resolvable and (now shouldVisitImplicitCode is
    // on) DEFINED even for an implicit/trivial destructor. The temporary
    // itself gets a FRESH, non-escaping Local identity scoped to this one
    // BindTemporaryExpr — sound, since an ordinary temporary's storage is
    // never observable outside the full-expression that created it
    // (barring the rare const&-lifetime-extension-then-leaked-elsewhere
    // case — an accepted, documented gap, same spirit as other escape-
    // tracking limitations already in this file). CK_TEMP_DTOR still
    // means "no operand of its own" for sequencing purposes (only
    // getSubExpr() is a real operand) — but the Go checker now consults
    // this node's CallSite fact via callEffectAccesses instead of
    // blanket alwaysTaint, same as CK_CALL/CK_CONSTRUCT_CALL, whenever
    // getDestructor() resolved (falls back to the old opaque behavior,
    // via callEffectAccesses's own cs==nil case, on the rare occasions
    // it doesn't).
    ck = facts_pb::CK_TEMP_DTOR;
    kids.push_back({BTE->getSubExpr(), false});
    if (CXXDestructorDecl *Dtor = const_cast<CXXDestructorDecl *>(
            BTE->getTemporary()->getDestructor())) {
      facts_pb::Aml tempAml;
      tempAml.mutable_local()->set_local_id(nextTempLocalId_++);
      // A bound temporary's most-derived type is statically known. Its
      // end-of-full-expression destructor is therefore the exact destructor
      // Clang attached to the temporary, even when that destructor is virtual.
      emitCallSite(Dtor, /*indirect=*/false, {}, fn, feid, id,
                   E->getExprLoc(), /*virtualMD=*/nullptr,
                   /*receiverExpr=*/nullptr, /*receiverIsArrow=*/false, Dtor,
                   &tempAml, /*sourceCall=*/BTE);
    }
  } else if (auto *CE = dyn_cast<CallExpr>(E)) {
    ck = facts_pb::CK_CALL;
    kids.push_back({CE->getCallee(), false});  // operand 0 = callee (convention)
    for (Expr *arg : CE->arguments()) kids.push_back({arg, false});
    FunctionDecl *calleeFD = CE->getDirectCallee();
    bool indirect = !calleeFD;
    CXXMethodDecl *virtualMD = nullptr;
    Expr *receiverExpr = nullptr;
    bool receiverIsArrow = false;
    CXXMethodDecl *methodForReceiver = nullptr;
    if (auto *MCE = dyn_cast<CXXMemberCallExpr>(CE)) {
      if (auto *MD = MCE->getMethodDecl()) {
        methodForReceiver = MD;
        receiverExpr = MCE->getImplicitObjectArgument();
        if (auto *ME = dyn_cast<MemberExpr>(MCE->getCallee()->IgnoreParenImpCasts()))
          receiverIsArrow = ME->isArrow();
        if (MD->isVirtual()) {
          bool performsVirtualDispatch = true;
          if (auto *ME = dyn_cast<MemberExpr>(
                  MCE->getCallee()->IgnoreParenImpCasts()))
            performsVirtualDispatch =
                ME->performsVirtualDispatch(Ctx.getLangOpts());
          if (performsVirtualDispatch) {
            emitDynamicTypeUse(receiverExpr, fn,
                               facts_pb::DynamicTypeUse::DTU_VIRTUAL_CALL,
                               E->getExprLoc());
            if (auto *Devirt = devirtualizedMethod(MD, receiverExpr)) {
              calleeFD = Devirt;
              methodForReceiver = Devirt;
              indirect = false;
            } else {
              indirect = true;
              virtualMD = MD;
            }
          } else {
            // A qualified call (`object.Base::f()`) suppresses virtual dispatch.
            indirect = false;
          }
        }
      }
    }
    std::vector<Expr *> callArgs(CE->arg_begin(), CE->arg_end());
    // Clang exposes va_start as __builtin_va_start, but does not make that
    // builtin declaration available to the translation-unit declaration
    // walk. Emitting a CallSite therefore creates a direct callee ID with no
    // FuncDecl fact and falls through to the undefined-callee top effect.
    // The dedicated handling below emits the relevant ParamRegion accesses,
    // so omit only this unnameable builtin call rather than modeling it as an
    // opaque external function.
    bool isBuiltinVaStart = calleeFD && !indirect &&
        calleeFD->getDeclName().isIdentifier() &&
        calleeFD->getName() == "__builtin_va_start";
    if (!isBuiltinVaStart)
      emitCallSite(indirect ? nullptr : calleeFD, indirect, callArgs, fn,
                   feid, id, E->getExprLoc(), virtualMD,
                   receiverExpr, receiverIsArrow, methodForReceiver,
                   /*rawReceiver=*/nullptr, /*sourceCall=*/CE,
                   /*calleeExpr=*/indirect ? CE->getCallee() : nullptr);
    // __builtin_va_start(ap, last_named): initializes a va_list so that
    // subsequent va_arg calls read through ALL named parameters of the
    // current function (the variadic arguments beyond the last named one
    // are also reachable, but they have no parameter index).  Emit
    // synthetic Access facts for each named parameter position to make
    // these reads visible in the function's footprint — without this,
    // VAArgExpr (which the extractor never visits) silently drops all
    // variadic reads, and the function's only visible effects come from
    // whatever else it calls (often an undefined callee like vprintf,
    // which injects W_UNKNOWN_GLOBAL).  The ParamRegion entries are
    // cross-function-nameable and remap precisely at call sites.
    if (isBuiltinVaStart) {
      for (unsigned pi = 0; pi < currentFunctionParamCount_; ++pi) {
        facts_pb::Fact af;
        facts_pb::Access *ac = af.mutable_access();
        *ac->mutable_function() = fn;
        ac->set_full_expr_id(feid);
        ac->set_node_id(id);
        ac->set_kind(facts_pb::Access::WRITE);
        ac->mutable_aml()->mutable_param_region()->set_param_index(pi);
        ac->mutable_aml()->mutable_param_region()->set_deref_depth(1);
        setLoc(ac->mutable_loc(), E->getExprLoc(), SM);
        af.set_origin(originOf(isSystemLoc(E->getExprLoc(), SM)));
        g_facts.push_back(std::move(af));
      }
    }
  } else if (isa<InitListExpr>(E)) {
    ck = facts_pb::CK_INIT_LIST;
    for (Stmt *c : E->children())
      if (auto *ce = dyn_cast_or_null<Expr>(c)) kids.push_back({ce, false});
  } else if (auto *NE = dyn_cast<CXXNewExpr>(E)) {
    // operand 0..N-2 = allocator's own args (placement args + array size),
    // indeterminate among themselves; operand N-1 (initializer), if
    // present, is sequenced after all of them (C++17+; see CK_NEW doc).
    ck = facts_pb::CK_NEW;
    for (Expr *pa : NE->placement_arguments()) kids.push_back({pa, false});
    if (NE->isArray())
      if (std::optional<Expr *> sz = NE->getArraySize())
        kids.push_back({*sz, false});
    if (Expr *init = NE->getInitializer()) kids.push_back({init, false});
  } else if (isa<CXXThrowExpr>(E)) {
    ck = facts_pb::CK_THROW;
    for (Stmt *c : E->children())
      if (auto *ce = dyn_cast_or_null<Expr>(c)) kids.push_back({ce, false});
  } else if (isa<CoawaitExpr>(E) || isa<CoyieldExpr>(E)) {
    ck = facts_pb::CK_CO_AWAIT;
    for (Stmt *c : E->children())
      if (auto *ce = dyn_cast_or_null<Expr>(c)) kids.push_back({ce, false});
  } else {
    // leaf-ish: still descend into any expr children (e.g. member base)
    for (Stmt *c : E->children())
      if (auto *ce = dyn_cast_or_null<Expr>(c)) kids.push_back({ce, false});
  }

  std::vector<uint64_t> operandIds;
  for (auto &kid : kids)
    operandIds.push_back(
        buildNode(kid.E, fn, feid, kid.writeCtx, kid.readModifyWrite, kid.discarded));

  facts_pb::Fact nf;
  facts_pb::ExprNode *en = nf.mutable_expr_node();
  *en->mutable_function() = fn;
  en->set_full_expr_id(feid);
  en->set_node_id(id);
  en->set_construct(ck);
  for (uint64_t oid : operandIds) en->add_operands(oid);
  setLoc(en->mutable_loc(), E->getExprLoc(), SM);
  nf.set_origin(originOf(isSystemLoc(E->getExprLoc(), SM)));
  g_facts.push_back(std::move(nf));

  // Access leaf? Volatile lvalues get the single collapsed VolatileLoc
  // (Rule 4.6.1's volatile clause: any two unsequenced volatile accesses
  // conflict, regardless of which object); otherwise a plain named
  // variable gets a real per-entity Aml (see computeNamedVarAml).
  facts_pb::Aml aml;
  bool isVolAccess = isVolatileLvalue(E);
  bool trackable = isVolAccess;
  if (isVolAccess) aml.mutable_volatile_loc();
  else trackable = computeNamedVarAml(E, &aml);
  if (trackable) {
    auto emitAccess = [&](facts_pb::Access::Kind k) {
      facts_pb::Fact af;
      facts_pb::Access *ac = af.mutable_access();
      *ac->mutable_function() = fn;
      ac->set_full_expr_id(feid);
      ac->set_node_id(id);
      ac->set_kind(k);
      *ac->mutable_aml() = aml;
      ac->set_is_volatile(isVolAccess);
      setLoc(ac->mutable_loc(), E->getExprLoc(), SM);
      af.set_origin(originOf(isSystemLoc(E->getExprLoc(), SM)));
      g_facts.push_back(std::move(af));
      // escaped is fixed up retroactively once the whole function has
      // been scanned (VisitFunctionDecl) — record where, in case a LATER
      // escape of the same local is discovered after this fact is emitted.
      if (aml.has_local() || aml.has_field())
        if (auto *VD = rootVarOfFieldChain(E))
          if (!VD->hasGlobalStorage())
            localAccessFactIdx_[VD].push_back(g_facts.size() - 1);
    };
    if (readModifyWrite) {
      emitAccess(facts_pb::Access::READ);
      emitAccess(facts_pb::Access::WRITE);
    } else if (writeCtx) {
      emitAccess(facts_pb::Access::WRITE);
    } else if (isRead) {
      emitAccess(facts_pb::Access::READ);
    } else if (discarded) {
      // (void)local / (void)local.field: a pure discard (see the
      // ExplicitCastExpr check above) — exposes nothing at all, not an
      // access and not an escape. Emit nothing.
    } else if (aml.has_local() || aml.has_field()) {
      // Neither written nor actually read: reference-binding or
      // pass-by-reference (no lvalue-to-rvalue conversion) — this local
      // (or the local rooting this field chain) is effectively exposed;
      // an opaque call elsewhere could then touch it through that alias.
      // (&local / &local.field is handled separately, via markEscapeIfLocal.)
      if (auto *VD = rootVarOfFieldChain(E))
        if (!VD->hasGlobalStorage())
          escapedLocals_.insert(VD);
    }
    // else (global, neither written nor read): merely naming an object
    // isn't an access; globals don't need escape tracking (a call can
    // already plausibly touch any global regardless).
  }
  return id;
}

bool FactVisitor::isVolatileLvalue(Expr *E) {
  if (auto *UO = dyn_cast<UnaryOperator>(E))
    if (UO->getOpcode() == UO_Deref)
      return E->getType().isVolatileQualified();
  if (!(isa<DeclRefExpr>(E) || isa<MemberExpr>(E) ||
        isa<ArraySubscriptExpr>(E)))
    return false;
  return E->getType().isVolatileQualified();
}

VarDecl *FactVisitor::rootVarOfFieldChain(Expr *E) {
  E = E->IgnoreParenImpCasts();
  if (auto *ME = dyn_cast<MemberExpr>(E))
    return ME->isArrow() ? nullptr : rootVarOfFieldChain(ME->getBase());
  if (auto *DRE = dyn_cast<DeclRefExpr>(E))
    return dyn_cast<VarDecl>(DRE->getDecl());
  return nullptr;
}

void FactVisitor::markEscapeIfLocal(Expr *E) {
  auto *VD = rootVarOfFieldChain(E);
  if (VD && !VD->hasGlobalStorage()) escapedLocals_.insert(VD);
}

bool FactVisitor::isTrackablePointerLocal(const VarDecl *VD) {
  return VD->getType()->isPointerType() && !VD->hasGlobalStorage() &&
         !isa<ParmVarDecl>(VD);
}

VarDecl *FactVisitor::trackablePointerVarOf(Expr *E) {
  auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
  if (!DRE) return nullptr;
  auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
  return (VD && isTrackablePointerLocal(VD)) ? VD : nullptr;
}

bool FactVisitor::paramRegionOf(Expr *E, unsigned &index, unsigned &depth) {
  E = E->IgnoreParenImpCasts();
  if (isa<CXXThisExpr>(E)) {
    index = currentFunctionParamCount_;
    depth = 0;
    return true;
  }
  if (auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() != UO_Deref) return false;
    if (!paramRegionOf(UO->getSubExpr(), index, depth)) return false;
    ++depth;
    return true;
  }
  if (auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
    if (!paramRegionOf(ASE->getBase(), index, depth)) return false;
    ++depth;
    return true;
  }
  auto *DRE = dyn_cast<DeclRefExpr>(E);
  if (!DRE) return false;
  auto *PVD = dyn_cast<ParmVarDecl>(DRE->getDecl());
  if (!PVD) return false;
  if (PVD->getType()->isReferenceType()) {
    index = PVD->getFunctionScopeIndex();
    depth = 1;
    return true;
  }
  if (PVD->getType()->isPointerType()) {
    index = PVD->getFunctionScopeIndex();
    depth = 0;
    return true;
  }
  return false;
}
