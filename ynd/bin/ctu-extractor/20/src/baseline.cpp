#include "baseline.h"
#include "baseline_pointer_proof.h"
#include "clang/AST/DeclCXX.h"
#include "source_utils.h"
#include "initialization_flow.h"

void emitBaselineCandidate(clang::ASTContext &ctx, llvm::StringRef rule,
                           clang::SourceLocation loc, llvm::StringRef reason,
                           bool coverageGap, const clang::Expr *read,
                           const clang::FunctionDecl *effectCallee) {
  auto &sm = ctx.getSourceManager();
  // Preserve otherwise unlocatable obligations at the translation-unit root.
  if (loc.isInvalid()) {
    loc = sm.getLocForStartOfFile(sm.getMainFileID());
    coverageGap = true;
  }
  facts_pb::Fact fact;
  fact.set_origin(originOf(isSystemLoc(loc, sm)));
  auto *candidate = fact.mutable_baseline_candidate();
  candidate->set_rule_id(rule.str());
  setLoc(candidate->mutable_loc(), loc, sm);
  candidate->set_reason(reason.str());
  candidate->set_coverage_gap(coverageGap);
  if (rule == "11.6.2" && (reason == "value-read-needs-initialization-proof" ||
                           reason == "read-modify-write-needs-initialization-proof" ||
                           reason == "aggregate-copy-needs-member-initialization-proof"))
    tagInitializationRead(read, candidate);
  if (effectCallee) {
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(effectCallee);
    if (!method || !method->isVirtual())
      computeFuncEntityId(const_cast<clang::FunctionDecl *>(effectCallee), ctx, sm,
                          candidate->mutable_effect_callee());
  }
  g_facts.push_back(std::move(fact));
}

namespace {
const clang::FunctionDecl *immutableFunctionTarget(const clang::Expr *expr,unsigned depth) {
  if(!expr || depth>16) return nullptr;
  expr=expr->IgnoreParenImpCasts();
  if(const auto *unary=clang::dyn_cast<clang::UnaryOperator>(expr)) {
    if((unary->getOpcode()==clang::UO_AddrOf && unary->getSubExpr()->getType()->isFunctionType()) ||
       (unary->getOpcode()==clang::UO_Deref && unary->getSubExpr()->getType()->isFunctionPointerType()))
      return immutableFunctionTarget(unary->getSubExpr(),depth+1);
    return nullptr;
  }
  const auto *ref=clang::dyn_cast<clang::DeclRefExpr>(expr);
  if(!ref) return nullptr;
  if(const auto *fn=clang::dyn_cast<clang::FunctionDecl>(ref->getDecl())) return fn;
  const auto *var=clang::dyn_cast<clang::VarDecl>(ref->getDecl());
  if(!var || !var->getType().isConstQualified() || var->getType().isVolatileQualified() ||
     !var->getType()->isFunctionPointerType() || !var->hasInit()) return nullptr;
  // A dynamic nonlocal initializer is not evidence of the pointer value at
  // an arbitrary call during program startup. Automatic aliases are bound
  // by their executed declaration; constant-initialized globals are stable.
  if(var->hasGlobalStorage() && !var->hasConstantInitialization()) return nullptr;
  return immutableFunctionTarget(var->getInit(),depth+1);
}
}
const clang::FunctionDecl *exactFunctionPointerTarget(const clang::VarDecl *var) {
  if(!var || !var->getType().isConstQualified() || var->getType().isVolatileQualified() ||
     !var->getType()->isFunctionPointerType() || !var->hasInit() ||
     (var->hasGlobalStorage() && !var->hasConstantInitialization())) return nullptr;
  return immutableFunctionTarget(var->getInit(),0);
}
const clang::FunctionDecl *exactDirectCallee(const clang::CallExpr *call) {
  if(!call) return nullptr;
  if(const auto *direct=call->getDirectCallee()) return direct;
  return immutableFunctionTarget(call->getCallee(),0);
}

// The scalar-return contracts of <cmath> also cover domain/range errors: a
// normal return supplies a value (possibly NaN/infinity), not indeterminate
// storage. Keep this separate from effects, UB, numeric ranges and invariants.
bool hasDefinedMathReturn(const clang::FunctionDecl *function, clang::ASTContext &ctx) {
  if (!function || !function->getIdentifier() || function->hasBody() ||
      function->isVariadic() || !function->getReturnType()->isRealFloatingType()) return false;
  auto name = function->getName();
  if (name.starts_with("__builtin_")) {
    if (!function->getBuiltinID()) return false;
    name = name.drop_front(10);
  } else if (!function->isExternC() ||
             !ctx.getSourceManager().isInSystemHeader(function->getLocation())) return false;
  for (const auto *parameter : function->parameters())
    if (!parameter->getType()->isRealFloatingType()) return false;
  static const char *const unary[] = {
      "sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh",
      "asinh", "acosh", "atanh", "exp", "exp2", "expm1", "log", "log10",
      "log2", "log1p", "sqrt", "cbrt", "fabs", "floor", "ceil", "trunc",
      "round", "nearbyint", "rint", "erf", "erfc", "tgamma", "lgamma"};
  static const char *const binary[] = {
      "atan2", "pow", "hypot", "fmod", "remainder", "fmin", "fmax", "fdim",
      "copysign", "nextafter", "nexttoward"};
  auto matches = [&](llvm::StringRef base) {
    QualType expected;
    if (name == base) expected = ctx.DoubleTy;
    else if (name.size() == base.size() + 1 && name.starts_with(base)) {
      if (name.back() == 'f') expected = ctx.FloatTy;
      else if (name.back() == 'l') expected = ctx.LongDoubleTy;
    }
    if (expected.isNull() || !ctx.hasSameUnqualifiedType(function->getReturnType(), expected)) return false;
    for (unsigned i = 0; i < function->getNumParams(); ++i) {
      auto argument = base == "nexttoward" && i == 1 ? ctx.LongDoubleTy : expected;
      if (!ctx.hasSameUnqualifiedType(function->getParamDecl(i)->getType(), argument)) return false;
    }
    return true;
  };
  if (function->getNumParams() == 1)
    for (auto base : unary) if (matches(base)) return true;
  if (function->getNumParams() == 2)
    for (auto base : binary) if (matches(base)) return true;
  return function->getNumParams() == 3 && matches("fma");
}

// Positive witnesses intentionally use a smaller grammar than the candidate
// engine. No failed safety proof is consulted here. Unknown control flow and
// calls stop execution inspection instead of joining possible effects.
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Attr.h"
#include "clang/Analysis/CFG.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"

void emitPositiveWitness(clang::ASTContext &ctx, llvm::StringRef rule, llvm::StringRef kind,
                         clang::SourceLocation loc, clang::SourceLocation evidence) {
  auto &sm = ctx.getSourceManager();
  if (loc.isInvalid() || evidence.isInvalid() || sm.isInSystemHeader(loc)) return;
  facts_pb::Fact fact;
  fact.set_origin(originOf(isSystemLoc(loc, sm)));
  auto *w = fact.mutable_baseline_candidate();
  w->set_rule_id(rule.str());
  w->set_violation_witness_version(1);
  w->set_violation_witness_kind(kind.str());
  // Keep potential-mode readers descriptive, without granting them proof.
  w->set_reason(kind.str());
  setLoc(w->mutable_loc(), loc, sm);
  setLoc(w->add_violation_witness_sites(), evidence, sm);
  g_facts.push_back(std::move(fact));
}

namespace {
class PositiveWitnesses : public clang::RecursiveASTVisitor<PositiveWitnesses> {
  clang::ASTContext &Context;
  llvm::DenseSet<const clang::ValueDecl *> Referenced;
  llvm::DenseSet<const clang::FunctionDecl *> RuntimeCalled;
  llvm::StringSet<> AsmLabels;
  bool OpaqueTU = false;
  bool Collecting = true;

  const clang::Expr *plain(const clang::Expr *e) const {
    return e ? e->IgnoreParenImpCasts() : nullptr;
  }
  const clang::VarDecl *variable(const clang::Expr *e) const {
    auto *ref = clang::dyn_cast_or_null<clang::DeclRefExpr>(plain(e));
    auto *v = ref ? clang::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
    return v ? v->getCanonicalDecl() : nullptr;
  }
  bool scalar(const clang::VarDecl *v) const {
    return v && v->getType()->isIntegerType() && !v->getType().isVolatileQualified();
  }
  void emit(llvm::StringRef rule, llvm::StringRef kind, clang::SourceLocation loc,
            clang::SourceLocation evidence, bool allowMacro = false) {
    auto &sm = Context.getSourceManager();
    if (loc.isInvalid() || evidence.isInvalid() || (!allowMacro && (loc.isMacroID() ||
        evidence.isMacroID())) || sm.isInSystemHeader(loc)) return;
    emitPositiveWitness(Context, rule, kind, loc, evidence);
  }

  const clang::VarDecl *array(const clang::Expr *e) const {
    const auto *v = variable(e);
    // Distinct automatic complete objects cannot be assembler aliases.
    return v && v->hasLocalStorage() && Context.getAsConstantArrayType(v->getType()) ? v : nullptr;
  }
  const clang::VarDecl *update(const clang::Expr *e) const {
    auto *u = clang::dyn_cast_or_null<clang::UnaryOperator>(plain(e));
    if (!u || !u->isIncrementDecrementOp()) return nullptr;
    auto *v = variable(u->getSubExpr());
    return scalar(v) ? v : nullptr;
  }
  bool constant(const clang::Expr *e, unsigned depth = 0) const {
    e = plain(e);
    if (!e || depth > 32 || e->isValueDependent()) return false;
    if (clang::isa<clang::IntegerLiteral, clang::CharacterLiteral,
                   clang::CXXBoolLiteralExpr, clang::CXXNullPtrLiteralExpr>(e)) return true;
    // Constant evaluation of a constexpr CALL is not a runtime value proof:
    // is_constant_evaluated can select a different runtime branch. Accept a
    // literal-only, side-effect-free grammar before asking Clang to evaluate it.
    if (!e->getType()->isIntegerType()) return false;
    if (const auto *u = clang::dyn_cast<clang::UnaryOperator>(e)) {
      auto op = u->getOpcode();
      return (op == clang::UO_Plus || op == clang::UO_Minus ||
              op == clang::UO_Not || op == clang::UO_LNot) &&
             constant(u->getSubExpr(), depth + 1) && e->isCXX11ConstantExpr(Context);
    }
    if (const auto *b = clang::dyn_cast<clang::BinaryOperator>(e))
      return !b->isAssignmentOp() && b->getOpcode() != clang::BO_Comma &&
             constant(b->getLHS(), depth + 1) && constant(b->getRHS(), depth + 1) &&
             e->isCXX11ConstantExpr(Context);
    return false;
  }
  // This grammar is only a controlling-expression proof. Do not reuse it for
  // memory/UB witnesses: constexpr evaluation alone is not runtime equivalence.
  bool invariantConstant(const clang::Expr *e, unsigned depth = 0) const {
    e = plain(e);
    if (!e || depth > 32 || e->isValueDependent() || !e->getType()->isIntegralOrEnumerationType()) return false;
    if (const auto *arg = clang::dyn_cast<clang::CXXDefaultArgExpr>(e))
      return invariantConstant(arg->getExpr(), depth + 1);
    if (constant(e)) return true;
    if (const auto *ref = clang::dyn_cast<clang::DeclRefExpr>(e))
      return clang::isa<clang::EnumConstantDecl>(ref->getDecl());
    if (const auto *u = clang::dyn_cast<clang::UnaryOperator>(e)) {
      auto op = u->getOpcode();
      return (op == clang::UO_Plus || op == clang::UO_Minus || op == clang::UO_Not || op == clang::UO_LNot) &&
             invariantConstant(u->getSubExpr(), depth + 1);
    }
    if (const auto *b = clang::dyn_cast<clang::BinaryOperator>(e))
      return !b->isAssignmentOp() && b->getOpcode() != clang::BO_Comma &&
             invariantConstant(b->getLHS(), depth + 1) && invariantConstant(b->getRHS(), depth + 1);
    const auto *call = clang::dyn_cast<clang::CallExpr>(e);
    const auto *callee = call ? call->getDirectCallee() : nullptr;
    const clang::FunctionDecl *definition = nullptr;
    if (!callee || !callee->isConstexpr() || clang::isa<clang::CXXMethodDecl>(callee) ||
        callee->isVariadic() || !callee->hasBody(definition) ||
        !callee->getReturnType()->isIntegerType() || call->getNumArgs() != callee->getNumParams()) return false;
    const auto *body = clang::dyn_cast<clang::CompoundStmt>(definition->getBody());
    if (!body || body->size() != 1) return false;
    const auto *ret = clang::dyn_cast<clang::ReturnStmt>(*body->body_begin());
    if (!ret || !ret->getRetValue()) return false;
    for (unsigned i = 0; i < call->getNumArgs(); ++i)
      if (!definition->getParamDecl(i)->getType()->isIntegerType() ||
          !invariantConstant(call->getArg(i), depth + 1)) return false;
    const auto *value = plain(ret->getRetValue());
    if (const auto *ref = clang::dyn_cast<clang::DeclRefExpr>(value))
      if (const auto *parameter = clang::dyn_cast<clang::ParmVarDecl>(ref->getDecl()))
        for (const auto *p : definition->parameters())
          if (parameter == p) return true;
    // No recursive callee-body evaluation, globals, builtins or hidden effects.
    return constant(value);
  }
  bool builtinSpecializationType(clang::QualType type, bool &fixed) const {
    type = type.getCanonicalType();
    if (type->isBuiltinType()) { fixed = true; return true; }
    if (type->getAs<clang::TemplateTypeParmType>()) return true;
    if (const auto *pointer = type->getAs<clang::PointerType>()) {
      // Pointer-to-dependent/user types requires type-definition provenance.
      auto pointee = pointer->getPointeeType().getCanonicalType();
      if (pointee->isBuiltinType()) { fixed = true; return true; }
    }
    return false;
  }
  bool literalArrayInitializer(const clang::Expr *e) const {
    const auto *list = clang::dyn_cast_or_null<clang::InitListExpr>(plain(e));
    if (!list) return false;
    for (const auto *item : list->inits())
      if (!clang::isa<clang::ImplicitValueInitExpr>(item) && !constant(item)) return false;
    return true;
  }
  const clang::VarDecl *singleVariable(const clang::Stmt *s) const {
    auto *d = clang::dyn_cast_or_null<clang::DeclStmt>(s);
    return d && d->isSingleDecl() ? clang::dyn_cast<clang::VarDecl>(d->getSingleDecl()) : nullptr;
  }
  const clang::CallExpr *libraryCall(const clang::Stmt *s, llvm::StringRef name) const {
    auto *expr = clang::dyn_cast_or_null<clang::Expr>(s);
    auto *call = clang::dyn_cast_or_null<clang::CallExpr>(plain(expr));
    auto *f = call ? call->getDirectCallee() : nullptr;
    if (!f || !f->getIdentifier() || f->getName() != name || f->hasBody() ||
        !f->isExternC() || !Context.getSourceManager().isInSystemHeader(f->getLocation())) return nullptr;
    return call;
  }
  bool usedOverload(const clang::FunctionDecl *function) const {
    // An assembler-name declaration can refer to this function without a
    // C++ DeclRefExpr. Unrelated libc redirects do not open every function.
    if (AsmLabels.count(mangleName(const_cast<clang::FunctionDecl *>(function), Context))) return true;
    for (const auto *decl : Referenced)
      if (const auto *other = clang::dyn_cast<clang::FunctionDecl>(decl))
        if (other->getDeclName() == function->getDeclName() &&
            other->getDeclContext()->getRedeclContext() == function->getDeclContext()->getRedeclContext()) return true;
    return false;
  }
  bool literalString(const clang::Expr *e) const {
    return clang::isa_and_nonnull<clang::StringLiteral>(plain(e));
  }
  void sequences(const clang::CompoundStmt *body, const clang::FunctionDecl *owner) {
    if (body->size() == 2) {
      auto i = body->body_begin();
      auto *write = libraryCall(*i++, "fputc");
      auto *read = libraryCall(*i, "fgetc");
      if (write && read && write->getNumArgs() == 2 && read->getNumArgs() == 1 &&
          constant(write->getArg(0))) {
        auto *a = variable(write->getArg(1)), *b = variable(read->getArg(0));
        if (a && a == b && clang::isa<clang::ParmVarDecl>(a) && a->getType()->isPointerType())
          emit("30.0.2", "adjacent-stream-direction-change", read->getExprLoc(), write->getExprLoc());
      }
    }
    if (body->size() != 3) return;
    auto i = body->body_begin();
    auto *pointer = singleVariable(*i++);
    const auto *middle = *i++;
    auto *result = clang::dyn_cast<clang::ReturnStmt>(*i);
    if (!pointer || !pointer->hasLocalStorage() || !pointer->getType()->isPointerType() || !result) return;
    if (pointer->hasInit()) {
      auto *first = libraryCall(pointer->getInit(), "getenv");
      auto *second = libraryCall(middle, "getenv");
      if (first && second && first->getNumArgs() == 1 && second->getNumArgs() == 1 &&
          literalString(first->getArg(0)) && literalString(second->getArg(0)) &&
          variable(result->getRetValue()) == pointer->getCanonicalDecl())
        emit("25.5.3", "library-pointer-after-repeat", result->getReturnLoc(), second->getExprLoc());
      return;
    }
    auto *scope = clang::dyn_cast<clang::CompoundStmt>(middle);
    if (!scope || scope->size() != 2) return;
    auto j = scope->body_begin();
    auto *local = singleVariable(*j++);
    auto *store = clang::dyn_cast<clang::BinaryOperator>(*j);
    auto *address = store ? clang::dyn_cast<clang::UnaryOperator>(plain(store->getRHS())) : nullptr;
    auto *load = clang::dyn_cast_or_null<clang::UnaryOperator>(plain(result->getRetValue()));
    if (scalar(local) && local->hasLocalStorage() && local->hasInit() && constant(local->getInit()) &&
        store && store->getOpcode() == clang::BO_Assign && variable(store->getLHS()) == pointer->getCanonicalDecl() &&
        address && address->getOpcode() == clang::UO_AddrOf && variable(address->getSubExpr()) == local->getCanonicalDecl() &&
        load && load->getOpcode() == clang::UO_Deref && variable(load->getSubExpr()) == pointer->getCanonicalDecl() &&
        owner->getReturnType()->isIntegerType())
      emit("6.8.1", "read-after-local-scope", result->getReturnLoc(), local->getLocation());
  }

  // A deliberately nonrecursive inspector: nested calls, branches and argument
  // evaluation cannot accidentally inherit a top-level execution certificate.
  void expression(const clang::Expr *e, const clang::FunctionDecl *owner,
                  const llvm::DenseSet<const clang::VarDecl *> &uninitialized) {
    e = plain(e);
    if (!e || e->isValueDependent()) return;
    if (const auto *b = clang::dyn_cast<clang::BinaryOperator>(e)) {
      const auto op = b->getOpcode();
      if (baseline_pointer::Proof(Context).overflowingSignedArithmetic(b))
        emit("4.1.3", "range-proven-signed-overflow", b->getOperatorLoc(), b->getExprLoc());
      if (baseline_pointer::Proof(Context).invalidShiftCount(b))
        emit("4.1.3", "range-proven-invalid-shift-count", b->getOperatorLoc(), b->getRHS()->getExprLoc());
      if (!(constant(b->getLHS()) && constant(b->getRHS())) && baseline_pointer::Proof(Context).zeroDivisor(b))
        emit("4.1.3", "range-proven-zero-divisor", b->getOperatorLoc(), b->getRHS()->getExprLoc());
      if ((op == clang::BO_Div || op == clang::BO_Rem) &&
          b->getType()->isIntegerType() && constant(b->getLHS())) {
        auto divisor = b->getRHS()->getIntegerConstantExpr(Context);
        if (constant(b->getRHS()) && divisor && divisor->isZero())
          emit("4.1.3", "constant-zero-divisor", b->getOperatorLoc(), b->getRHS()->getExprLoc());
      }
      if (op == clang::BO_Add || op == clang::BO_Sub || op == clang::BO_Mul ||
          op == clang::BO_Xor || op == clang::BO_And || op == clang::BO_Or) {
        auto *a = update(b->getLHS()), *c = update(b->getRHS());
        if (a && a == c && !uninitialized.count(a))
          emit("4.6.1", "unsequenced-scalar-updates", b->getOperatorLoc(), a->getLocation());
      }
      if (op == clang::BO_Add) {
        auto *v = array(b->getLHS());
        auto offset = b->getRHS()->getIntegerConstantExpr(Context);
        if (v && constant(b->getRHS()) && offset && (offset->isNegative() || offset->getLimitedValue() >
            Context.getAsConstantArrayType(v->getType())->getSize().getLimitedValue()))
          emit("8.7.1", "constant-array-pointer-outside", b->getOperatorLoc(), v->getLocation());
      }
      // Keep the legacy literal witness unique; other forms share the same
      // stable offset range engine as arithmetic and invariant-condition proofs.
      if (!(op==clang::BO_Add && array(b->getLHS()) && constant(b->getRHS())))
        if (const auto *owner=baseline_pointer::Proof(Context).outsideArrayArithmetic(b))
          emit("8.7.1", "range-proven-array-pointer-outside", b->getOperatorLoc(), owner->getLocation());
      auto *a = array(b->getLHS()), *c = array(b->getRHS());
      if (a && c && a != c) {
        if (op == clang::BO_Sub)
          emit("8.7.2", "distinct-array-subtraction", b->getOperatorLoc(), a->getLocation());
        if (b->isRelationalOp())
          emit("8.9.1", "distinct-array-order", b->getOperatorLoc(), a->getLocation());
      }
      if (b->isLogicalOp() && constant(b->getLHS())) {
        bool lhs;
        if (b->getLHS()->EvaluateAsBooleanCondition(lhs, Context) &&
            ((op == clang::BO_LAnd && lhs) || (op == clang::BO_LOr && !lhs))) {
          auto *rhs = clang::dyn_cast<clang::BinaryOperator>(plain(b->getRHS()));
          auto *v = rhs ? variable(rhs->getLHS()) : nullptr;
          if (rhs && rhs->getOpcode() == clang::BO_Assign && scalar(v) &&
              v->hasGlobalStorage() && constant(rhs->getRHS()))
            emit("8.14.1", "logical-rhs-global-write", rhs->getOperatorLoc(), v->getLocation());
        }
      }
    }
    if (const auto *call = clang::dyn_cast<clang::CallExpr>(e)) {
      auto *callee = call->getDirectCallee();
      if (callee && callee->getBuiltinID() && callee->getName() == "__builtin_memcpy" && call->getNumArgs() == 3) {
        auto *destination = array(call->getArg(0));
        auto *source = clang::dyn_cast<clang::BinaryOperator>(plain(call->getArg(1)));
        auto count = call->getArg(2)->getIntegerConstantExpr(Context);
        if (destination && source && source->getOpcode() == clang::BO_Add &&
            array(source->getLHS()) == destination && constant(call->getArg(2)) &&
            constant(source->getRHS()) && count && !count->isNegative()) {
          auto offset = source->getRHS()->getIntegerConstantExpr(Context);
          const auto *type = Context.getAsConstantArrayType(destination->getType());
          auto size = type->getSize().getLimitedValue();
          auto bytes = count->getLimitedValue();
          if (type->getElementType()->isCharType() && !type->getElementType().isConstQualified() &&
              offset && !offset->isNegative() && offset->getLimitedValue() > 0 &&
              offset->getLimitedValue() < bytes && bytes <= size && offset->getLimitedValue() <= size - bytes)
            emit("8.18.1", "overlapping-builtin-memcpy", call->getExprLoc(), destination->getLocation());
        }
      }
      if (callee && callee->getIdentifier() && callee->getName() == "all_of" &&
          Context.getSourceManager().isInSystemHeader(callee->getLocation()) && call->getNumArgs() == 3) {
        const auto *space = clang::dyn_cast<clang::NamespaceDecl>(callee->getDeclContext());
        auto *a = array(call->getArg(0));
        auto *end = clang::dyn_cast<clang::BinaryOperator>(plain(call->getArg(1)));
        auto *lambda = clang::dyn_cast<clang::LambdaExpr>(plain(call->getArg(2)));
        auto *body = lambda ? clang::dyn_cast<clang::CompoundStmt>(lambda->getBody()) : nullptr;
        if (space && space->isStdNamespace() && a && end && end->getOpcode() == clang::BO_Add &&
            array(end->getLHS()) == a && body && body->size() == 2 && lambda->capture_size() == 0 &&
            a->hasInit() && literalArrayInitializer(a->getInit()) && constant(end->getRHS())) {
          auto count = end->getRHS()->getIntegerConstantExpr(Context);
          auto j = body->body_begin();
          auto *increment = clang::dyn_cast<clang::UnaryOperator>(*j++);
          auto *result = clang::dyn_cast<clang::ReturnStmt>(*j);
          auto *v = increment ? variable(increment->getSubExpr()) : nullptr;
          bool value = false;
          if (count && !count->isNegative() && count->getLimitedValue() > 0 && count->getLimitedValue() <=
              Context.getAsConstantArrayType(a->getType())->getSize().getLimitedValue() &&
              increment && increment->isIncrementDecrementOp() && scalar(v) && v->hasGlobalStorage() &&
              v->getType()->isUnsignedIntegerType() && result && constant(result->getRetValue()) &&
              result->getRetValue()->EvaluateAsBooleanCondition(value, Context) && value)
            emit("28.3.1", "executed-predicate-global-update", call->getExprLoc(), increment->getOperatorLoc());
        }
      }

    }
    if (const auto *throwExpr = clang::dyn_cast<clang::CXXThrowExpr>(e)) {
      auto *prototype = owner->getType()->getAs<clang::FunctionProtoType>();
      if (prototype && prototype->isNothrow() && constant(throwExpr->getSubExpr()) &&
          throwExpr->getSubExpr()->getType()->isIntegerType())
        emit("18.5.1", "uncaught-noexcept-throw", e->getExprLoc(), owner->getLocation());
    }
  }
  bool closedPrivateAccess(const clang::CXXRecordDecl *record) const {
    if (record->isDependentContext()) return false;
    if (record->isLocalClass() || record->isInAnonymousNamespace()) return true;
    // External linkage alone need not leave private access open: if all
    // access-capable bodies are present, an outside TU cannot add a new one
    // without violating the ODR. Friends/templates/nested classes leave this
    // certificate open and require whole-program reference aggregation.
    for (const auto *decl : record->decls()) {
      if (clang::isa<clang::FriendDecl, clang::FunctionTemplateDecl, clang::ClassTemplateDecl>(decl)) return false;
      if (const auto *nested = clang::dyn_cast<clang::CXXRecordDecl>(decl))
        if (!nested->isImplicit()) return false;
      if (const auto *method = clang::dyn_cast<clang::CXXMethodDecl>(decl))
        if (!method->isImplicit() && !method->isDeleted() && !method->isDefaulted() && !method->hasBody()) return false;
    }
    return true;
  }
  // These rules constrain call relationships, not termination or UB. Use
  // evaluated CFG call elements (not a recursive AST walk) so deferred lambda
  // bodies, unevaluated operands, discarded branches and noreturn successors
  // do not supply an edge. Canonical declarations distinguish specializations.
  void callRelationships(clang::FunctionDecl *owner) {
    // CFG body construction does not establish entry through base/member
    // initializers. Keep the original conservative constructor boundary.
    if (const auto *ctor = clang::dyn_cast<clang::CXXConstructorDecl>(owner))
      if (ctor->getNumCtorInitializers() || ctor->getParent()->getNumBases()) return;
    clang::CFG::BuildOptions options;
    options.PruneTriviallyFalseEdges = true;
    options.setAlwaysAdd(clang::Stmt::CallExprClass);
    options.setAlwaysAdd(clang::Stmt::CXXMemberCallExprClass);
    options.setAlwaysAdd(clang::Stmt::CXXOperatorCallExprClass);
    auto cfg = clang::CFG::buildCFG(owner, owner->getBody(), &Context, options);
    if (!cfg) return;
    llvm::DenseSet<const clang::CFGBlock *> seen;
    llvm::SmallVector<const clang::CFGBlock *, 16> pending{&cfg->getEntry()};
    while (!pending.empty()) {
      auto *block = pending.pop_back_val();
      if (!seen.insert(block).second) continue;
      for (const auto &element : *block) {
        auto statement = element.getAs<clang::CFGStmt>();
        const auto *call = statement ? clang::dyn_cast<clang::CallExpr>(statement->getStmt()) : nullptr;
        if (!call) continue;
        const auto *callee = exactDirectCallee(call);
        const auto *method = clang::dyn_cast_or_null<clang::CXXMethodDecl>(callee);
        const auto *memberCall = clang::dyn_cast<clang::CXXMemberCallExpr>(call);
        const auto *member = memberCall ? clang::dyn_cast<clang::MemberExpr>(plain(memberCall->getCallee())) : nullptr;
        if (Collecting) {
          if (callee && !owner->isConstexpr() && !call->isValueDependent() &&
              !call->isCXX11ConstantExpr(Context))
            RuntimeCalled.insert(callee->getCanonicalDecl());
          continue;
        }
        // A virtual self-reference does not prove its dynamic target.
        if (callee && callee->getCanonicalDecl() == owner->getCanonicalDecl() &&
            (!method || !method->isVirtual() || (member && member->hasQualifier()) ||
             method->hasAttr<clang::FinalAttr>() || method->getParent()->hasAttr<clang::FinalAttr>()) &&
            (!owner->isConstexpr() || RuntimeCalled.count(owner->getCanonicalDecl())))
          emit("8.2.10", "direct-self-call", call->getExprLoc(), owner->getLocation());
        if (member && !member->hasQualifier() && method && method->isVirtual() &&
            clang::isa<clang::CXXThisExpr>(plain(memberCall->getImplicitObjectArgument())) &&
            (clang::isa<clang::CXXConstructorDecl>(owner) || clang::isa<clang::CXXDestructorDecl>(owner)))
          emit("15.1.1", "ctor-this-virtual-call", call->getExprLoc(), owner->getLocation());
      }
      for (auto successor : block->succs())
        if (const auto *next = successor.getReachableBlock()) pending.push_back(next);
    }
  }
 public:
  explicit PositiveWitnesses(clang::ASTContext &context) : Context(context) {}
  bool shouldVisitTemplateInstantiations() const { return true; }
  bool VisitDeclRefExpr(clang::DeclRefExpr *ref) {
    if (Collecting) Referenced.insert(clang::cast<clang::ValueDecl>(ref->getDecl()->getCanonicalDecl()));
    return true;
  }
  bool VisitMemberExpr(clang::MemberExpr *ref) {
    if (Collecting) Referenced.insert(clang::cast<clang::ValueDecl>(ref->getMemberDecl()->getCanonicalDecl()));
    return true;
  }
  bool VisitClassTemplateSpecializationDecl(clang::ClassTemplateSpecializationDecl *spec) {
    if (Collecting || !spec->isThisDeclarationADefinition() ||
        spec->getSpecializationKind() != clang::TSK_ExplicitSpecialization) return true;
    auto &sm = Context.getSourceManager();
    auto location = sm.getExpansionLoc(spec->getLocation());
    if (location.isInvalid() || sm.isInSystemHeader(location)) return true;
    const auto *file = sm.getFileEntryForID(sm.getFileID(location));
    if (!file) return true;
    // A defaulted void enable_if slot is not evidence that the type being
    // specialized is builtin. Leave dependent leading types to provenance.
    const auto args = spec->getTemplateArgs().asArray();
    bool leadingFixed = false;
    if (args.empty() || args.front().getKind() != clang::TemplateArgument::Type ||
        !builtinSpecializationType(args.front().getAsType(), leadingFixed) || !leadingFixed) return true;
    bool fixed = false;
    for (const auto &arg : args)
      if (arg.getKind() != clang::TemplateArgument::Type ||
          !builtinSpecializationType(arg.getAsType(), fixed)) return true;
    if (!fixed) return true;
    const auto *primary = spec->getSpecializedTemplate();
    if (!primary) return true;
    for (const auto *decl : primary->redecls()) {
      auto site = sm.getExpansionLoc(decl->getLocation());
      if (site.isInvalid()) return true;
      const auto *primaryFile = sm.getFileEntryForID(sm.getFileID(site));
      if (!primaryFile || primaryFile == file) return true;
    }
    emit("6.2.3", "builtin-template-specialization-placement", location,
         sm.getExpansionLoc(primary->getLocation()), true);
    return true;
  }
  bool VisitIfStmt(clang::IfStmt *statement) {
    if (!Collecting && !statement->isConstexpr() && !statement->getInit() &&
        !statement->getConditionVariable() && invariantConstant(statement->getCond()) &&
        statement->getCond()->isCXX11ConstantExpr(Context))
      emit("0.0.2", "invariant-pure-forwarding-condition", statement->getIfLoc(),
           statement->getCond()->getExprLoc(), true);
    else if (!Collecting && !statement->isConstexpr() && !statement->getInit() &&
             !statement->getConditionVariable()) {
      const auto *comparison = clang::dyn_cast<clang::BinaryOperator>(plain(statement->getCond()));
      if (comparison && baseline_pointer::Proof(Context).invariantComparison(comparison))
        emit("0.0.2", "invariant-stable-range-condition", statement->getIfLoc(),
             comparison->getExprLoc(), true);
    }
    return true;
  }
  bool VisitSwitchStmt(clang::SwitchStmt *statement) {
    if (!Collecting && !statement->getInit() && !statement->getConditionVariable() && constant(statement->getCond()))
      emit("0.0.2", "constant-switch-control", statement->getSwitchLoc(), statement->getCond()->getExprLoc(), true);
    return true;
  }
  bool VisitAsmStmt(clang::AsmStmt *) { OpaqueTU = true; return true; }
  bool VisitFileScopeAsmDecl(clang::FileScopeAsmDecl *) { OpaqueTU = true; return true; }
  bool VisitFunctionDecl(clang::FunctionDecl *f) {
    if (Collecting) {
      if (f->hasAttr<clang::AliasAttr>() || f->hasAttr<clang::WeakRefAttr>()) OpaqueTU = true;
      if (const auto *label = f->getAttr<clang::AsmLabelAttr>()) AsmLabels.insert(label->getLabel());
      if (f->doesThisDeclarationHaveABody() && !f->isDependentContext() &&
          !f->getDescribedFunctionTemplate() && !Context.getSourceManager().isInSystemHeader(f->getLocation()))
        callRelationships(f);
      return true;
    }
    if (!f->doesThisDeclarationHaveABody() || f->isImplicit() || f->isDependentContext() ||
        f->isDefaulted() || f->getDescribedFunctionTemplate() ||
        Context.getSourceManager().isInSystemHeader(f->getLocation())) return true;
    auto *body = clang::dyn_cast<clang::CompoundStmt>(f->getBody());
    if (!body) return true; // function-try-block has a handler
    if (!OpaqueTU && f->getFormalLinkage() == clang::Linkage::Internal &&
        !clang::isa<clang::CXXMethodDecl>(f) && !f->hasAttrs() && !f->isInlined() &&
        !usedOverload(f))
      emit("0.2.4", "unreferenced-internal-function", f->getLocation(), body->getLBracLoc());
    if (!OpaqueTU) {
      const auto *method = clang::dyn_cast<clang::CXXMethodDecl>(f);
      if (method && !f->isTemplated() && method->getAccess() == clang::AS_private && !method->isVirtual() &&
          !clang::isa<clang::CXXConstructorDecl, clang::CXXDestructorDecl, clang::CXXConversionDecl>(method) &&
          !method->isCopyAssignmentOperator() && !method->isMoveAssignmentOperator() &&
          closedPrivateAccess(method->getParent()) &&
          !f->hasAttrs() && !f->isReferenced() && !usedOverload(f))
        emit("0.2.4", "unreferenced-closed-private-method", f->getLocation(), body->getLBracLoc());
    }
    callRelationships(f);
    // Constant evaluation, templates and coroutines require separate execution
    // models. The following witnesses concern ordinary function entry only.
    if (f->isConstexpr() || f->isTemplated()) return true;
    // Constructor initializers execute before the body. Their calls can
    // terminate or throw; this baseline has no entry proof through them.
    if (const auto *ctor = clang::dyn_cast<clang::CXXConstructorDecl>(f))
      if (ctor->getNumCtorInitializers() != 0 || ctor->getParent()->getNumBases() != 0) return true;
    sequences(body, f);
    if (f->isNoReturn() && body->body_empty() && f->getReturnType()->isVoidType() &&
        !clang::isa<clang::CXXMethodDecl>(f))
      emit("9.6.4", "noreturn-normal-return", f->getLocation(), body->getRBracLoc());
    llvm::DenseSet<const clang::VarDecl *> uninitialized;
    llvm::DenseSet<const clang::VarDecl *> initialized;
    for (const auto *statement : body->body()) {
      if (const auto *decls = clang::dyn_cast<clang::DeclStmt>(statement)) {
        bool supported = true;
        for (const auto *decl : decls->decls()) {
          const auto *v = clang::dyn_cast<clang::VarDecl>(decl);
          if (!v || !v->hasLocalStorage() || v->getType()->isReferenceType() ||
              (!scalar(v) && !Context.getAsConstantArrayType(v->getType()))) { supported = false; break; }
          if (scalar(v)) {
            if (!v->hasInit()) uninitialized.insert(v->getCanonicalDecl());
            else if (!constant(v->getInit())) { supported = false; break; }
            if (v->hasInit() && constant(v->getInit())) initialized.insert(v->getCanonicalDecl());
          } else if (v->hasInit() && v->getInit()->HasSideEffects(Context)) { supported = false; break; }
        }
        if (!supported) break;
        continue;
      }
      if (const auto *condition = clang::dyn_cast<clang::IfStmt>(statement)) {
        const auto *v = variable(condition->getCond());
        if (!condition->isConstexpr() && !condition->getInit() && !condition->getConditionVariable() &&
            scalar(v) && initialized.count(v) && !constant(condition->getCond()))
          emit("0.0.2", "fixed-private-scalar-condition", condition->getIfLoc(), v->getLocation());
        break;
      }
      if (const auto *ret = clang::dyn_cast<clang::ReturnStmt>(statement)) {
        if (f->isNoReturn() && !clang::isa<clang::CXXMethodDecl>(f) &&
            (!ret->getRetValue() || constant(ret->getRetValue())))
          emit("9.6.4", "noreturn-normal-return", ret->getReturnLoc(), f->getLocation());
        auto *v = variable(ret->getRetValue());
        if (scalar(v) && uninitialized.count(v) && f->getReturnType()->isIntegerType())
          emit("11.6.2", "uninitialized-scalar-return", ret->getReturnLoc(), v->getLocation());
        expression(ret->getRetValue(), f, uninitialized);
        break;
      }
      if (const auto *e = clang::dyn_cast<clang::Expr>(statement)) {
        expression(e, f, uninitialized);
        // Any expression may mutate state or fail to return. No later witness
        // is certified until we have an exact transfer model for it.
      }
      break;
    }
    return true;
  }
  bool VisitStmtExpr(clang::StmtExpr *e) {
    if (!Collecting) emit("4.1.1", "gnu-statement-expression", e->getLParenLoc(), e->getRParenLoc());
    return true;
  }
  void run() {
    TraverseDecl(Context.getTranslationUnitDecl());
    Collecting = false;
    TraverseDecl(Context.getTranslationUnitDecl());
  }
};
}
void collectPositiveWitnessFacts(clang::ASTContext &context) {
  PositiveWitnesses(context).run();
}
