// Conservative review candidates, not proofs of MISRA violations. In particular,
// a candidate is retained when reachability, liveness or effects are unknown.
#include "baseline.h"
#include "baseline_pointer_proof.h"
#include "baseline_lifetime_proof.h"
#include "baseline_library_proof.h"
#include "baseline_context_proof.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/EvaluatedExprVisitor.h"
#include "clang/Analysis/Analyses/LiveVariables.h"
#include "clang/Analysis/CFG.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DenseMap.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"

namespace {
// The CFG liveness callback observes values after a statement (before the
// backwards transfer). A scalar write is suspicious only when no successor
// path reads its stored value. Escaped/aggregate storage remains an obligation.
class DeadStoreObserver : public clang::LiveVariables::Observer {
 public:
  explicit DeadStoreObserver(clang::ASTContext &context) : Context(context) {}
  void report(const clang::VarDecl *variable, clang::SourceLocation location,
              const clang::LiveVariables::LivenessValues &live) {
    if (!variable || variable->isImplicit() || !variable->hasLocalStorage() ||
        !variable->getType()->isScalarType() ||
        variable->getType().isVolatileQualified() || live.isLive(variable)) return;
    emitBaselineCandidate(Context, "0.1.1", location,
        "Local scalar store has no subsequent value read in the function CFG; escaped aliases require review");
  }
  void observeStmt(const clang::Stmt *statement, const clang::CFGBlock *,
                   const clang::LiveVariables::LivenessValues &live) override {
    if (const auto *declarations = clang::dyn_cast<clang::DeclStmt>(statement)) {
      for (const auto *declaration : declarations->decls())
        if (const auto *variable = clang::dyn_cast<clang::VarDecl>(declaration))
          if (variable->hasInit()) report(variable, variable->getLocation(), live);
    }
    if (const auto *assignment = clang::dyn_cast<clang::BinaryOperator>(statement)) {
      if (assignment->isAssignmentOp())
        if (const auto *reference = clang::dyn_cast<clang::DeclRefExpr>(
                assignment->getLHS()->IgnoreParenImpCasts()))
          report(clang::dyn_cast<clang::VarDecl>(reference->getDecl()),
                 assignment->getOperatorLoc(), live);
    }
    if (const auto *update = clang::dyn_cast<clang::UnaryOperator>(statement))
      if (update->isIncrementDecrementOp())
        if (const auto *reference = clang::dyn_cast<clang::DeclRefExpr>(
                update->getSubExpr()->IgnoreParenImpCasts()))
          report(clang::dyn_cast<clang::VarDecl>(reference->getDecl()),
                 update->getOperatorLoc(), live);
  }
 private:
  clang::ASTContext &Context;
};

bool inStandardNamespace(const clang::DeclContext *context);

// A literal is a valid null-terminated character sequence. The standard
// character-traits string_view constructor only determines its length and
// stores the pointer; its signature need not carry noexcept in the library.
// Do not model user traits or arbitrary pointers as nonthrowing.
bool literalStringViewConstruction(const clang::CXXConstructExpr *expression) {
  if (expression->getNumArgs() != 1 ||
      !clang::isa<clang::StringLiteral>(expression->getArg(0)->IgnoreParenImpCasts())) return false;
  const auto *specialization = clang::dyn_cast<clang::ClassTemplateSpecializationDecl>(
      expression->getConstructor()->getParent());
  if (!specialization || specialization->getName() != "basic_string_view" ||
      !inStandardNamespace(specialization->getDeclContext())) return false;
  const auto &arguments = specialization->getTemplateArgs();
  if (arguments.size() != 2 || arguments[0].getKind() != clang::TemplateArgument::Type ||
      arguments[1].getKind() != clang::TemplateArgument::Type ||
      !arguments[0].getAsType()->isAnyCharacterType()) return false;
  const auto *traits = clang::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
      arguments[1].getAsType()->getAsCXXRecordDecl());
  return traits && traits->getName() == "char_traits" &&
      inStandardNamespace(traits->getDeclContext()) && traits->getTemplateArgs().size() == 1 &&
      traits->getTemplateArgs()[0].getKind() == clang::TemplateArgument::Type &&
      traits->getTemplateArgs()[0].getAsType().getCanonicalType() ==
          arguments[0].getAsType().getCanonicalType();
}

// Check the visible implementation as well as the standard method identity:
// these query signatures need not have noexcept, and an implementation may
// supply extra behavior beyond its specified return value.
bool nonThrowingStreamQuery(const clang::FunctionDecl *function, unsigned depth = 0);
bool nonThrowingQueryExpression(const clang::Stmt *statement, unsigned depth) {
  if (!statement || depth > 48) return false;
  if (const auto *call = clang::dyn_cast<clang::CallExpr>(statement)) {
    if (!nonThrowingStreamQuery(exactDirectCallee(call), depth + 1)) return false;
  } else if (const auto *cast = clang::dyn_cast<clang::ExplicitCastExpr>(statement)) {
    if (!cast->getType()->isIntegralOrEnumerationType() ||
        !cast->getSubExpr()->getType()->isIntegralOrEnumerationType()) return false;
  } else if (!clang::isa<clang::DeclRefExpr, clang::MemberExpr, clang::CXXThisExpr,
                        clang::IntegerLiteral, clang::CXXBoolLiteralExpr,
                        clang::ImplicitCastExpr, clang::ParenExpr,
                        clang::UnaryOperator, clang::BinaryOperator>(statement)) return false;
  for (const auto *child : statement->children())
    if (child && !nonThrowingQueryExpression(child, depth + 1)) return false;
  return true;
}
bool nonThrowingStreamQuery(const clang::FunctionDecl *function, unsigned depth) {
  const auto *method = clang::dyn_cast_or_null<clang::CXXMethodDecl>(function);
  if (!method) {
    // libstdc++ represents iostate bit operations with visible enum operators.
    if (!function || depth > 48 || !inStandardNamespace(function->getDeclContext()) ||
        !function->getReturnType()->isEnumeralType() ||
        (function->getOverloadedOperator() != clang::OO_Amp &&
         function->getOverloadedOperator() != clang::OO_Pipe &&
         function->getOverloadedOperator() != clang::OO_Caret &&
         function->getOverloadedOperator() != clang::OO_Tilde)) return false;
    const auto *body = clang::dyn_cast_or_null<clang::CompoundStmt>(function->getBody());
    if (!body || body->size() != 1) return false;
    const auto *result = clang::dyn_cast<clang::ReturnStmt>(*body->body_begin());
    return result && nonThrowingQueryExpression(result->getRetValue(), depth + 1);
  }
  if (depth > 48 || method->isVirtual() || method->getNumParams() != 0 ||
      !inStandardNamespace(method->getDeclContext()) ||
      (method->getParent()->getName() != "basic_ios" && method->getParent()->getName() != "ios_base")) return false;
  const auto name = method->getNameAsString();
  const auto *conversion = clang::dyn_cast<clang::CXXConversionDecl>(method);
  if (!(conversion && conversion->getConversionType()->isBooleanType()) &&
      name != "good" && name != "eof" && name != "fail" && name != "bad" && name != "rdstate" &&
      method->getOverloadedOperator() != clang::OO_Exclaim) return false;
  const auto *body = clang::dyn_cast_or_null<clang::CompoundStmt>(method->getBody());
  if (!body || body->size() != 1) return false;
  const auto *result = clang::dyn_cast<clang::ReturnStmt>(*body->body_begin());
  return result && nonThrowingQueryExpression(result->getRetValue(), depth + 1);
}

class ThrowingBodyVisitor : public clang::EvaluatedExprVisitor<ThrowingBodyVisitor> {
 public:
  explicit ThrowingBodyVisitor(clang::ASTContext &context)
      : EvaluatedExprVisitor(context) {}
  bool MayThrow = false;
  static bool potentiallyThrowing(const clang::FunctionDecl *function) {
    if (!function) return true;
    const auto *prototype = function->getType()->getAs<clang::FunctionProtoType>();
    return !prototype || prototype->canThrow() != clang::CT_Cannot;
  }
  void VisitCallExpr(clang::CallExpr *expression) {
    if (expression->isUnevaluatedBuiltinCall(Context)) return;
    const clang::FunctionProtoType *prototype = nullptr;
    if (const auto *callee = expression->getDirectCallee())
      prototype = callee->getType()->getAs<clang::FunctionProtoType>();
    else {
      clang::QualType type = expression->getCallee()->getType();
      if (type->isPointerType() || type->isReferenceType()) type = type->getPointeeType();
      prototype = type->getAs<clang::FunctionProtoType>();
    }
    MayThrow |= (!prototype || prototype->canThrow() != clang::CT_Cannot) &&
                !nonThrowingStreamQuery(expression->getDirectCallee());
    VisitStmt(expression);
  }
  void VisitCXXConstructExpr(clang::CXXConstructExpr *expression) {
    MayThrow |= !literalStringViewConstruction(expression) &&
                potentiallyThrowing(expression->getConstructor());
    VisitStmt(expression);
  }
  void VisitCXXNewExpr(clang::CXXNewExpr *expression) {
    MayThrow |= potentiallyThrowing(expression->getOperatorNew());
    VisitStmt(expression);
  }
  void VisitCXXThrowExpr(clang::CXXThrowExpr *expression) {
    MayThrow = true;
    VisitStmt(expression);
  }
  void VisitCXXDynamicCastExpr(clang::CXXDynamicCastExpr *expression) {
    MayThrow |= expression->getTypeAsWritten()->isReferenceType();
    VisitStmt(expression);
  }
  void VisitCXXTypeidExpr(clang::CXXTypeidExpr *expression) {
    MayThrow |= expression->isPotentiallyEvaluated();
    EvaluatedExprVisitor::VisitCXXTypeidExpr(expression);
  }
};

bool inStandardNamespace(const clang::DeclContext *context) {
  for (; context; context = context->getParent())
    if (const auto *space = clang::dyn_cast<clang::NamespaceDecl>(context))
      if (space->isStdNamespace()) return true;
  return false;
}

bool standardCharacterRecord(const clang::CXXRecordDecl *record) {
  const auto *specialization = clang::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(record);
  if (!specialization || !inStandardNamespace(record->getDeclContext()) ||
      (record->getName() != "basic_string" && record->getName() != "basic_string_view")) return false;
  const auto &arguments = specialization->getTemplateArgs();
  if (arguments.size() < 2 || arguments[0].getKind() != clang::TemplateArgument::Type ||
      arguments[1].getKind() != clang::TemplateArgument::Type || !arguments[0].getAsType()->isAnyCharacterType()) return false;
  const auto *traits = clang::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
      arguments[1].getAsType()->getAsCXXRecordDecl());
  return traits && traits->getName() == "char_traits" && inStandardNamespace(traits->getDeclContext()) &&
      traits->getTemplateArgs().size() == 1 && traits->getTemplateArgs()[0].getKind() == clang::TemplateArgument::Type &&
      traits->getTemplateArgs()[0].getAsType().getCanonicalType() == arguments[0].getAsType().getCanonicalType();
}

// Persistent-effect proof for a small set of specified library queries. Const
// qualification alone is not a proof: arbitrary user methods can mutate globals.
class PureExpressions {
 public:
  explicit PureExpressions(clang::ASTContext &context) : Context(context) {}
  bool callback(const clang::Expr *argument, unsigned depth = 0) {
    if (!argument || depth > 12 || argument->getType().isNull()) return false;
    const auto *record = argument->getType().getNonReferenceType()->getAsCXXRecordDecl();
    // In an uninstantiated enclosing template, auto has not yet been deduced
    // even when its initializer is an independent, explicit lambda.
    if (!record)
      if (const auto *reference = clang::dyn_cast<clang::DeclRefExpr>(argument->IgnoreParenImpCasts()))
        if (const auto *variable = clang::dyn_cast<clang::VarDecl>(reference->getDecl()))
          if (variable->getType().isConstQualified() && variable->hasInit())
            if (const auto *lambda = clang::dyn_cast<clang::LambdaExpr>(variable->getInit()->IgnoreParenImpCasts()))
              record = lambda->getLambdaClass();
    if (!record || !record->isLambda()) return false;
    // Captureless closure copy/destruction cannot carry user effects, including
    // the primary AST of an otherwise unrelated enclosing function template.
    if (record->capture_size() != 0 && (!record->isTriviallyCopyable() || !record->hasTrivialDestructor())) return false;
    const auto *method = record->getLambdaCallOperator();
    const auto *body = method ? clang::dyn_cast_or_null<clang::CompoundStmt>(method->getBody()) : nullptr;
    if (!body || body->size() != 1) return false;
    const auto *ret = clang::dyn_cast<clang::ReturnStmt>(*body->body_begin());
    return ret && expression(ret->getRetValue(), depth + 1);
  }
  bool expression(const clang::Expr *expr, unsigned depth = 0) {
    if (!expr || depth > 12 || expr->getType().isNull()) return false;
    if (!expr->HasSideEffects(Context)) return true;
    expr = expr->IgnoreParens();
    if (const auto *cast = clang::dyn_cast<clang::CastExpr>(expr))
      return !(cast->getCastKind() == clang::CK_LValueToRValue &&
               cast->getSubExpr()->getType().isVolatileQualified()) &&
          !cast->getType().isVolatileQualified() && expression(cast->getSubExpr(), depth + 1);
    if (const auto *binary = clang::dyn_cast<clang::BinaryOperator>(expr))
      return !binary->isAssignmentOp() && expression(binary->getLHS(), depth + 1) && expression(binary->getRHS(), depth + 1);
    if (const auto *unary = clang::dyn_cast<clang::UnaryOperator>(expr))
      return !unary->isIncrementDecrementOp() && expression(unary->getSubExpr(), depth + 1);
    const auto *call = clang::dyn_cast<clang::CallExpr>(expr);
    const auto *callee = call ? exactDirectCallee(call) : nullptr;
    if (!callee) {
      const auto *lookup = call ? clang::dyn_cast<clang::UnresolvedLookupExpr>(call->getCallee()->IgnoreParenImpCasts()) : nullptr;
      if (!lookup || lookup->requiresADL() || call->getNumArgs() != 3 ||
          !lookup->getName().isIdentifier() || lookup->getName().getAsString() != "all_of") return false;
      bool standardAlgorithm = false;
      for (const auto *declaration : lookup->decls()) {
        const auto *templ = clang::dyn_cast<clang::FunctionTemplateDecl>(declaration);
        const auto *function = templ ? templ->getTemplatedDecl() : clang::dyn_cast<clang::FunctionDecl>(declaration);
        if (!function || !inStandardNamespace(function->getDeclContext()) ||
            !Context.getSourceManager().isInSystemHeader(function->getLocation())) return false;
        standardAlgorithm |= function->getNumParams() == 3;
      }
      if (!standardAlgorithm || !call->getArg(0)->getType()->isPointerType() ||
          !call->getArg(1)->getType()->isPointerType() || !callback(call->getArg(2), depth + 1)) return false;
      return expression(call->getArg(0), depth + 1) && expression(call->getArg(1), depth + 1) &&
          expression(call->getArg(2), depth + 1);
    }
    if (!inStandardNamespace(callee->getDeclContext())) return false;
    bool known = false;
    if (const auto *member = clang::dyn_cast<clang::CXXMemberCallExpr>(call)) {
      const auto *method = member->getMethodDecl();
      if (!method || method->isVirtual() || !expression(member->getImplicitObjectArgument(), depth + 1)) return false;
      const auto name = method->getNameAsString();
      const auto owner = method->getParent()->getName();
      if (standardCharacterRecord(method->getParent()))
        known = name == "size" || name == "length" || name == "empty" || name == "data" ||
                name == "c_str" || name == "front" || name == "back" || name == "begin" || name == "end";
      if (owner == "basic_ios" || owner == "ios_base")
        known = name == "eof" || name == "fail" || name == "bad" || name == "good" || name == "rdstate";
    } else if (callee->isOverloadedOperator() && callee->getOverloadedOperator() == clang::OO_EqualEqual && call->getNumArgs() == 2) {
      // Standard string comparison delegates to traits; custom traits may have
      // arbitrary persistent effects and therefore are not covered.
      for (const auto *argument : call->arguments())
        if (const auto *record = argument->getType().getNonReferenceType()->getAsCXXRecordDecl()) {
          if (!standardCharacterRecord(record)) return false;
          known = true;
        }
    } else if (callee->getIdentifier() && callee->getName() == "all_of" && call->getNumArgs() == 3) {
      known = call->getArg(0)->getType()->isPointerType() && call->getArg(1)->getType()->isPointerType() &&
          callback(call->getArg(2), depth + 1);
    }
    if (!known) return false;
    for (const auto *argument : call->arguments())
      if (!expression(argument, depth + 1)) return false;
    return true;
  }
 private:
  clang::ASTContext &Context;
};

bool standardErrnoPointer(const clang::Expr *expr, clang::ASTContext &context) {
  const auto *call = clang::dyn_cast<clang::CallExpr>(expr->IgnoreParenImpCasts());
  const auto *callee = call ? exactDirectCallee(call) : nullptr;
  return callee && !callee->hasBody() && callee->getIdentifier() && callee->getName() == "__errno_location" &&
      callee->isExternC() && context.getSourceManager().isInSystemHeader(callee->getLocation());
}

bool mayBeCallable(clang::QualType type) {
  type = type.getNonReferenceType();
  if (type->isDependentType()) return true;
  if (type->isPointerType()) return type->getPointeeType()->isFunctionType();
  if (type->isFunctionType()) return true;
  if (const auto *record = type->getAsCXXRecordDecl()) {
    if (!record->hasDefinition()) return true;
    if (record->getNumBases()) return true; // inherited operator() or conversion
    for (const auto *method : record->methods())
      if (method->getOverloadedOperator() == clang::OO_Call) return true;
  }
  return false;
}

// A do-loop condition is not evaluated when every path through its body
// terminates. Reject nonlocal control flow/continue conservatively before
// recognizing a final unconditional terminator.
class ConditionJumpVisitor : public clang::RecursiveASTVisitor<ConditionJumpVisitor> {
 public:
  bool HasJump = false;
  bool VisitContinueStmt(clang::ContinueStmt *) { HasJump = true; return true; }
  bool VisitGotoStmt(clang::GotoStmt *) { HasJump = true; return true; }
  bool VisitIndirectGotoStmt(clang::IndirectGotoStmt *) { HasJump = true; return true; }
  bool VisitLabelStmt(clang::LabelStmt *) { HasJump = true; return true; }
};
bool cannotReachDoCondition(clang::Stmt *body) {
  ConditionJumpVisitor jumps;
  jumps.TraverseStmt(body);
  if (jumps.HasJump) return false;
  const clang::Stmt *last = body;
  while (const auto *compound = clang::dyn_cast<clang::CompoundStmt>(last)) {
    if (compound->body_empty()) return false;
    last = compound->body_back();
  }
  if (clang::isa<clang::ReturnStmt>(last) || clang::isa<clang::CXXThrowExpr>(last))
    return true;
  if (const auto *expression = clang::dyn_cast<clang::Expr>(last))
    if (const auto *call = clang::dyn_cast<clang::CallExpr>(expression->IgnoreParenImpCasts()))
      if (const auto *callee = exactDirectCallee(call))
        return callee->isNoReturn() ||
            (callee->getBuiltinID() && callee->getName() == "__builtin_unreachable");
  return false;
}

// Count evaluated references outside a function's own body. isReferenced also
// includes unevaluated decltype/sizeof, and isUsed includes self recursion.
class EvaluatedFunctionUses : public clang::EvaluatedExprVisitor<EvaluatedFunctionUses> {
 public:
  EvaluatedFunctionUses(clang::ASTContext &context,
      llvm::DenseSet<const clang::FunctionDecl *> &used,
      const clang::FunctionDecl *owner)
      : EvaluatedExprVisitor(context), Used(used), Owner(owner) {}
  void VisitDeclRefExpr(clang::DeclRefExpr *expression) {
    if (const auto *function = clang::dyn_cast<clang::FunctionDecl>(expression->getDecl()))
      if (!Owner || function->getCanonicalDecl() != Owner->getCanonicalDecl())
        Used.insert(function->getCanonicalDecl());
  }
  void VisitMemberExpr(clang::MemberExpr *expression) {
    if (const auto *function = clang::dyn_cast<clang::FunctionDecl>(expression->getMemberDecl()))
      if (!Owner || function->getCanonicalDecl() != Owner->getCanonicalDecl())
        Used.insert(function->getCanonicalDecl());
    VisitStmt(expression);
  }
 private:
  llvm::DenseSet<const clang::FunctionDecl *> &Used;
  const clang::FunctionDecl *Owner;
};
class FunctionUseCollector : public clang::RecursiveASTVisitor<FunctionUseCollector> {
 public:
  explicit FunctionUseCollector(clang::ASTContext &context) : Context(context) {}
  bool shouldVisitImplicitCode() const { return true; }
  bool shouldVisitTemplateInstantiations() const { return true; }
  llvm::DenseSet<const clang::FunctionDecl *> Used;
  llvm::DenseMap<const clang::FunctionDecl *, llvm::DenseSet<const clang::FunctionDecl *>> Edges;
  bool VisitFunctionDecl(clang::FunctionDecl *function) {
    if (!function->doesThisDeclarationHaveABody()) return true;
    const auto *canonical = function->getCanonicalDecl();
    const auto *method = clang::dyn_cast<clang::CXXMethodDecl>(function);
    if ((function->isExternallyVisible() && (!method || method->getAccess() != clang::AS_private)) ||
        function->hasAttr<clang::ConstructorAttr>() || function->hasAttr<clang::DestructorAttr>())
      Used.insert(canonical);
    EvaluatedFunctionUses uses(Context, Edges[canonical], function);
    uses.Visit(function->getBody());
    if (auto *constructor = clang::dyn_cast<clang::CXXConstructorDecl>(function))
      for (auto *initializer : constructor->inits()) uses.Visit(initializer->getInit());
    return true;
  }
  bool VisitVarDecl(clang::VarDecl *variable) {
    if (variable->isFileVarDecl() && variable->hasInit()) {
      EvaluatedFunctionUses uses(Context, Used, nullptr);
      uses.Visit(variable->getInit());
    }
    return true;
  }
  void resolveReachability() {
    llvm::SmallVector<const clang::FunctionDecl *, 32> pending(Used.begin(), Used.end());
    while (!pending.empty()) {
      const auto *function = pending.pop_back_val();
      auto found = Edges.find(function);
      if (found == Edges.end()) continue;
      for (const auto *callee : found->second)
        if (Used.insert(callee).second) pending.push_back(callee);
    }
  }
 private:
  clang::ASTContext &Context;
};

// A dependent template expression can retain BinaryOperator syntax even
// when a known class operand requires overload resolution. Without an implicit
// conversion, such an operand cannot participate in built-in arithmetic. Do
// not report arithmetic UB for the unresolved operator syntax itself; effects
// inside the eventual overloaded function remain separate analysis sites.
bool requiresOverloadedArithmetic(const clang::Expr *operand) {
  const auto type = operand->getType();
  if (type->isDependentType()) return false;
  const auto *record = type->getAsCXXRecordDecl();
  if (!record || !(record = record->getDefinition()) || record->isDependentContext()) return false;
  for (const auto &entry : record->getVisibleConversionFunctions()) {
    const auto *conversion = clang::dyn_cast<clang::CXXConversionDecl>(entry);
    // Unknown/templates and implicit (including inherited) conversions could
    // still select a built-in operator after template substitution.
    if (!conversion || !conversion->isExplicit()) return false;
  }
  return true;
}

class SemanticBaselineVisitor
    : public clang::RecursiveASTVisitor<SemanticBaselineVisitor> {
 public:
  explicit SemanticBaselineVisitor(clang::ASTContext &context,
      const llvm::DenseSet<const clang::FunctionDecl *> &used,
      const baseline_context::Proof &contexts) : Context(context), Used(used), Contexts(contexts) {}
  bool shouldVisitTemplateInstantiations() const { return true; }
  bool shouldVisitImplicitCode() const { return true; }

  // Keep traversing syntax for language-conformance diagnostics, but do not
  // infer runtime effects from unevaluated operands. Invoke the base traversal
  // without a data-recursion queue so the guard covers all descendants.
#define UNEVALUATED_TRAVERSAL(Node) \
  bool Traverse##Node(clang::Node *node) { \
    llvm::SaveAndRestore<bool> guard(InUnevaluated, true); \
    return clang::RecursiveASTVisitor<SemanticBaselineVisitor>::Traverse##Node(node); \
  }
  UNEVALUATED_TRAVERSAL(UnaryExprOrTypeTraitExpr)
  UNEVALUATED_TRAVERSAL(CXXNoexceptExpr)
  UNEVALUATED_TRAVERSAL(ExpressionTraitExpr)
  UNEVALUATED_TRAVERSAL(CXXUuidofExpr)
  UNEVALUATED_TRAVERSAL(RequiresExpr)
#undef UNEVALUATED_TRAVERSAL

  bool TraverseDecltypeTypeLoc(clang::DecltypeTypeLoc type) {
    llvm::SaveAndRestore<bool> guard(InUnevaluated, true);
    return clang::RecursiveASTVisitor<SemanticBaselineVisitor>::TraverseDecltypeTypeLoc(type);
  }
  bool TraverseTypeOfExprTypeLoc(clang::TypeOfExprTypeLoc type) {
    llvm::SaveAndRestore<bool> guard(InUnevaluated, true);
    return clang::RecursiveASTVisitor<SemanticBaselineVisitor>::TraverseTypeOfExprTypeLoc(type);
  }
  bool TraverseCXXTypeidExpr(clang::CXXTypeidExpr *expression) {
    llvm::SaveAndRestore<bool> guard(InUnevaluated,
        InUnevaluated || !expression->isPotentiallyEvaluated());
    return clang::RecursiveASTVisitor<SemanticBaselineVisitor>::TraverseCXXTypeidExpr(expression);
  }

  bool TraverseStaticAssertDecl(clang::StaticAssertDecl *declaration) {
    const bool previous = InStaticAssert;
    InStaticAssert = true;
    const bool result = clang::RecursiveASTVisitor<SemanticBaselineVisitor>::TraverseStaticAssertDecl(declaration);
    InStaticAssert = previous;
    return result;
  }

  void candidate(llvm::StringRef rule, clang::SourceLocation location,
                 llvm::StringRef reason, bool gap = false,
                 const clang::FunctionDecl *effectCallee = nullptr) {
    if (InUnevaluated && rule != "4.1.1") return;
    emitBaselineCandidate(Context, rule, location, reason, gap, nullptr, effectCallee);
  }

  void condition(const clang::Expr *expression) {
    if (expression && !Contexts.varies(expression))
      candidate("0.0.2", expression->getExprLoc(),
                "Controlling expression may be invariant; path-sensitive and cross-TU value proof is unavailable");
  }
  bool VisitIfStmt(clang::IfStmt *statement) {
    condition(statement->getCond());
    return true;
  }
  bool VisitWhileStmt(clang::WhileStmt *statement) {
    condition(statement->getCond());
    return true;
  }
  bool VisitDoStmt(clang::DoStmt *statement) {
    if (!cannotReachDoCondition(statement->getBody())) condition(statement->getCond());
    return true;
  }
  bool VisitForStmt(clang::ForStmt *statement) {
    condition(statement->getCond());
    return true;
  }
  bool VisitSwitchStmt(clang::SwitchStmt *statement) {
    condition(statement->getCond());
    return true;
  }
  bool VisitConditionalOperator(clang::ConditionalOperator *expression) {
    condition(expression->getCond());
    return true;
  }
  bool VisitBinaryConditionalOperator(clang::BinaryConditionalOperator *expression) {
    condition(expression->getCond());
    candidate("4.1.1", expression->getExprLoc(),
              "GNU omitted-middle conditional expression is outside ISO C++17");
    return true;
  }
  bool VisitStmtExpr(clang::StmtExpr *expression) {
    candidate("4.1.1", expression->getExprLoc(),
              "GNU statement expression is outside ISO C++17");
    return true;
  }
  bool VisitVarDecl(clang::VarDecl *declaration) {
    if (declaration->getType()->isVariablyModifiedType())
      candidate("4.1.1", declaration->getLocation(),
                "Variably modified object type is outside ISO C++17");
    return true;
  }
  bool VisitFunctionDecl(clang::FunctionDecl *declaration) {
    if (!declaration->doesThisDeclarationHaveABody() || declaration->isImplicit() ||
        declaration->isDefaulted()) return true;
    if (Context.getSourceManager().isInSystemHeader(declaration->getLocation())) return true;
    const auto *method = clang::dyn_cast<clang::CXXMethodDecl>(declaration);
    const bool limited = !declaration->isExternallyVisible() ||
                         (method && method->getAccess() == clang::AS_private);
    if (limited && !Used.contains(declaration->getCanonicalDecl()) &&
        !declaration->hasAttr<clang::ConstructorAttr>() &&
        !declaration->hasAttr<clang::DestructorAttr>() &&
        !declaration->getDescribedFunctionTemplate())
      candidate("0.2.4", declaration->getLocation(),
                "Limited-visibility function has no observed use or reference from a reachable TU root; external registration requires review");
    if (declaration->isDependentContext()) return true;
    clang::AnalysisDeclContextManager manager(Context, false, true, false, true);
    manager.getCFGBuildOptions().setAllAlwaysAdd();
    auto *analysis = manager.getContext(declaration);
    if (auto live = clang::LiveVariables::computeLiveness(*analysis, true)) {
      DeadStoreObserver observer(Context);
      live->runOnAllBlocks(observer);
    } else {
      candidate("0.1.1", declaration->getLocation(),
                "Coverage obligation: CFG liveness could not be computed for this function", true);
    }
    const auto *prototype = declaration->getType()->getAs<clang::FunctionProtoType>();
    if (prototype && prototype->isNothrow()) {
      ThrowingBodyVisitor throwing(Context);
      throwing.Visit(declaration->getBody());
      if (auto *constructor = clang::dyn_cast<clang::CXXConstructorDecl>(declaration))
        for (auto *initializer : constructor->inits()) throwing.Visit(initializer->getInit());
      if (const auto *cfg = analysis->getCFG()) {
        for (const auto *block : *cfg)
          for (const auto &element : *block)
            if (auto destructor = element.getAs<clang::CFGImplicitDtor>())
              throwing.MayThrow |= ThrowingBodyVisitor::potentiallyThrowing(
                  destructor->getDestructorDecl(Context));
      } else {
        throwing.MayThrow = true;
      }
      if (throwing.MayThrow)
        candidate("18.5.1", declaration->getLocation(),
                  "Potentially throwing operation or implicit destruction in noexcept body requires exception-path review");
    }
    return true;
  }
  bool VisitBinaryOperator(clang::BinaryOperator *expression) {
    // These are sites needing value/range/lifetime proofs, not claims that
    // overflow or another undefined behavior actually occurs on every path.
    const bool constant = !expression->isValueDependent() &&
                          expression->isCXX11ConstantExpr(Context);
    clang::QualType computationType = expression->getType();
    if (const auto *compound = clang::dyn_cast<clang::CompoundAssignOperator>(expression))
      computationType = compound->getComputationResultType();
    const bool unsignedArithmetic = computationType->isUnsignedIntegerType();
    const bool overloaded = expression->isTypeDependent() &&
        (requiresOverloadedArithmetic(expression->getLHS()) ||
         requiresOverloadedArithmetic(expression->getRHS()));
    if (!constant && !overloaded) switch (expression->getOpcode()) {
      case clang::BO_Div: case clang::BO_Rem:
      case clang::BO_DivAssign: case clang::BO_RemAssign: {
        auto divisor = expression->getRHS()->getIntegerConstantExpr(Context);
        // For signed division, only -1 can overflow (minimum / -1).
        if (divisor && !divisor->isZero() &&
            (unsignedArithmetic || !divisor->isAllOnes())) break;
        candidate("4.1.3", expression->getOperatorLoc(),
                  "Division or remainder requires a nonzero divisor and representable result proof");
        break;
      }
      case clang::BO_Shl: case clang::BO_Shr:
      case clang::BO_ShlAssign: case clang::BO_ShrAssign:
        if (expression->getOpcode() == clang::BO_Shr &&
            expression->getLHS()->IgnoreParenImpCasts()->getType()->isUnsignedIntegerType()) {
          auto amount = expression->getRHS()->getIntegerConstantExpr(Context);
          if (amount && !amount->isNegative() &&
              amount->getLimitedValue() < Context.getIntWidth(expression->getLHS()->getType())) break;
        }
        candidate("4.1.3", expression->getOperatorLoc(),
                  "Shift requires valid count and operand-range proofs under C++17 rules");
        break;
      case clang::BO_Add: case clang::BO_Sub: case clang::BO_Mul:
      case clang::BO_AddAssign: case clang::BO_SubAssign: case clang::BO_MulAssign:
        if (unsignedArithmetic) break; // modulo arithmetic is defined by C++
        candidate("4.1.3", expression->getOperatorLoc(),
                  "Arithmetic requires representability or pointer-bound proofs; undefined behavior is not excluded");
        break;
      default: break;
    }
    if (!InStaticAssert && expression->isLogicalOp() && !PureExpressions(Context).expression(expression->getRHS())) {
      // A callee summary covers only the call, not effects in its arguments or
      // receiver. Only anchor a plain call with independently effect-free args.
      const clang::FunctionDecl *effectCallee = nullptr;
      const auto *call = llvm::dyn_cast<clang::CallExpr>(expression->getRHS()->IgnoreParenImpCasts());
      if (call && !llvm::isa<clang::CXXMemberCallExpr>(call) &&
          !llvm::isa<clang::CXXOperatorCallExpr>(call)) {
        bool safeArguments = true;
        for (const auto *arg : call->arguments())
          safeArguments = safeArguments && !arg->HasSideEffects(Context);
        if (safeArguments) effectCallee = exactDirectCallee(call);
      }
      candidate("8.14.1", expression->getRHS()->getExprLoc(),
                "Logical RHS may have persistent side effects; transitive effects are not proven absent", false, effectCallee);
    }
    return true;
  }
  llvm::DenseMap<const clang::FunctionDecl *, bool> LiteralRangeOwnerSafety;
  bool literalRangeOwnerSafe(const clang::CXXForRangeStmt *loop) {
    // Resolve the actual enclosing function rather than retaining mutable
    // traversal state: visiting a local class or lambda must not reset its owner.
    clang::DynTypedNode node = clang::DynTypedNode::create(*loop);
    const clang::FunctionDecl *owner = nullptr;
    for (unsigned depth = 0; depth != 256; ++depth) {
      auto parents = Context.getParents(node);
      if (parents.size() != 1) return false;
      if ((owner = parents[0].get<clang::FunctionDecl>())) break;
      node = parents[0];
    }
    if (!owner || !owner->hasBody()) return false;
    auto cached = LiteralRangeOwnerSafety.find(owner);
    if (cached != LiteralRangeOwnerSafety.end()) return cached->second;
    class Scan : public clang::RecursiveASTVisitor<Scan> {
     public:
      bool Safe = true;
      bool shouldVisitImplicitCode() const { return true; }
      bool VisitStmt(clang::Stmt *statement) {
        if (clang::isa<clang::AsmStmt, clang::IndirectGotoStmt,
                       clang::CoroutineBodyStmt>(statement)) Safe = false;
        return Safe;
      }
      bool VisitCallExpr(clang::CallExpr *call) {
        const auto *callee = exactDirectCallee(call);
        if (!callee) {
          Safe = false;
          return false;
        }
        const auto *identifier = callee->getIdentifier();
        const auto name = identifier ? identifier->getName() : llvm::StringRef();
        if (callee->hasAttr<clang::ReturnsTwiceAttr>() || name == "setjmp" ||
            name == "_setjmp" || name == "sigsetjmp" || name == "__sigsetjmp" ||
            name == "__builtin_setjmp") Safe = false;
        return Safe;
      }
    } scan;
    scan.TraverseStmt(owner->getBody());
    LiteralRangeOwnerSafety[owner] = scan.Safe;
    return scan.Safe;
  }
  llvm::DenseSet<const clang::UnaryOperator *> SafeLiteralRangeOperations;
  bool VisitCXXForRangeStmt(clang::CXXForRangeStmt *loop) {
    if (!literalRangeOwnerSafe(loop)) return true;
    // Prove only the compiler-owned operations of a direct literal-list range.
    // The unnamed backing array lives through the loop; copying scalar elements
    // cannot expose its storage. No source-written iterator operation is covered.
    const auto *variable = loop->getLoopVariable();
    if (!variable || variable->getType()->isReferenceType() ||
        !variable->getType()->isScalarType()) return true;
    const clang::Expr *range = loop->getRangeInit();
    while (range) {
      range = range->IgnoreParenImpCasts();
      if (const auto *cleanup = clang::dyn_cast<clang::ExprWithCleanups>(range))
        range = cleanup->getSubExpr();
      else if (const auto *temporary = clang::dyn_cast<clang::MaterializeTemporaryExpr>(range))
        range = temporary->getSubExpr();
      else break;
    }
    const auto *list = clang::dyn_cast_or_null<clang::CXXStdInitializerListExpr>(range);
    if (!list) return true;
    const auto *record = clang::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
        list->getType()->getAsCXXRecordDecl());
    const auto *primary = record ? record->getSpecializedTemplate() : nullptr;
    if (!primary || primary->getName() != "initializer_list" ||
        !Context.getSourceManager().isInSystemHeader(primary->getLocation()) ||
        !Context.getSourceManager().isInSystemHeader(record->getLocation())) return true;
    const clang::DeclContext *space = primary->getDeclContext();
    while (const auto *ns = clang::dyn_cast<clang::NamespaceDecl>(space)) {
      if (!ns->isInline()) break;
      space = ns->getParent();
    }
    const auto *standard = clang::dyn_cast<clang::NamespaceDecl>(space);
    if (!standard || standard->getName() != "std" ||
        !standard->getParent()->isTranslationUnit()) return true;
    const clang::Expr *array = list->getSubExpr()->IgnoreParenImpCasts();
    if (const auto *temporary = clang::dyn_cast<clang::MaterializeTemporaryExpr>(array))
      array = temporary->getSubExpr()->IgnoreParenImpCasts();
    const auto *initializers = clang::dyn_cast<clang::InitListExpr>(array);
    if (!initializers || !Context.getAsConstantArrayType(initializers->getType())) return true;
    for (const auto *element : initializers->inits()) {
      const auto *literal = element->IgnoreParenImpCasts();
      if (!clang::isa<clang::IntegerLiteral, clang::FloatingLiteral,
                      clang::CharacterLiteral, clang::CXXBoolLiteralExpr,
                      clang::CXXNullPtrLiteralExpr, clang::StringLiteral>(literal)) return true;
    }
    auto declaration = [](const clang::DeclStmt *statement) -> const clang::VarDecl * {
      return statement && statement->isSingleDecl()
                 ? clang::dyn_cast<clang::VarDecl>(statement->getSingleDecl()) : nullptr;
    };
    const auto *begin = declaration(loop->getBeginStmt());
    const auto *end = declaration(loop->getEndStmt());
    if (!begin || !end || !begin->isImplicit() || !end->isImplicit() ||
        !begin->getType()->isPointerType() || !end->getType()->isPointerType()) return true;
    auto standardEndpoint = [&](const clang::VarDecl *decl, llvm::StringRef name) {
      const auto *call = decl->hasInit()
          ? clang::dyn_cast<clang::CXXMemberCallExpr>(decl->getInit()->IgnoreParenImpCasts()) : nullptr;
      const auto *method = call ? call->getMethodDecl() : nullptr;
      return method && method->getName() == name && method->isConst() &&
          call->getNumArgs() == 0 &&
          method->getParent()->getCanonicalDecl() == record->getCanonicalDecl() &&
          Context.getSourceManager().isInSystemHeader(method->getLocation());
    };
    if (!standardEndpoint(begin, "begin") || !standardEndpoint(end, "end")) return true;
    auto references = [](const clang::Expr *expr, const clang::VarDecl *decl) {
      const auto *ref = expr ? clang::dyn_cast<clang::DeclRefExpr>(expr->IgnoreParenImpCasts()) : nullptr;
      return ref && ref->getDecl() == decl;
    };
    const auto *condition = clang::dyn_cast_or_null<clang::BinaryOperator>(loop->getCond());
    const auto *increment = clang::dyn_cast_or_null<clang::UnaryOperator>(loop->getInc());
    const auto *read = variable->hasInit()
        ? clang::dyn_cast<clang::UnaryOperator>(variable->getInit()->IgnoreParenImpCasts()) : nullptr;
    if (!condition || condition->getOpcode() != clang::BO_NE ||
        !references(condition->getLHS(), begin) || !references(condition->getRHS(), end) ||
        !increment || increment->getOpcode() != clang::UO_PreInc ||
        !references(increment->getSubExpr(), begin) || !read ||
        read->getOpcode() != clang::UO_Deref || !references(read->getSubExpr(), begin)) return true;
    SafeLiteralRangeOperations.insert(increment);
    SafeLiteralRangeOperations.insert(read);
    return true;
  }
  bool VisitUnaryOperator(clang::UnaryOperator *expression) {
    if (SafeLiteralRangeOperations.count(expression)) return true;
    if (expression->getOpcode() == clang::UO_Deref &&
        !clang::isa<clang::CXXThisExpr>(expression->getSubExpr()->IgnoreParenImpCasts()) &&
        !standardErrnoPointer(expression->getSubExpr(), Context))
      candidate("4.1.3", expression->getOperatorLoc(),
                "Dereference requires valid pointer, object lifetime and alignment proofs");
    if ((expression->getOpcode() == clang::UO_Minus || expression->isIncrementDecrementOp()) &&
        !expression->getType()->isUnsignedIntegerType() &&
        !baseline_pointer::Proof(Context).safeIntegerIncrement(expression) &&
        (expression->isValueDependent() || !expression->isCXX11ConstantExpr(Context)))
      candidate("4.1.3", expression->getOperatorLoc(),
                "Unary arithmetic requires representability or pointer-bound proofs");
    return true;
  }
  bool VisitArraySubscriptExpr(clang::ArraySubscriptExpr *expression) {
    if (clang::isa<clang::StringLiteral>(expression->getBase()->IgnoreParenImpCasts()) &&
        baseline_pointer::Proof(Context).boundedSubscript(expression)) return true;
    const auto *array = Context.getAsConstantArrayType(
        expression->getBase()->IgnoreParenImpCasts()->getType());
    // A bounded terminator write through the original local character array
    // also needs a nonescape/lifetime certificate. Do not generalize this to
    // element reads, whose initialized-value obligations are independent.
    const auto parents = Context.getParents(*expression);
    const auto *assignment = parents.size() == 1 ? parents[0].get<clang::BinaryOperator>() : nullptr;
    if (array && Context.getBaseElementType(array->getElementType())->isCharType() &&
        assignment && assignment->getOpcode() == clang::BO_Assign && assignment->getLHS() == expression &&
        baseline_pointer::Proof(Context).boundedSubscript(expression) &&
        baseline_lifetime::Proof(Context).live(expression->getBase()->IgnoreParenImpCasts())) return true;
    auto index = expression->getIdx()->getIntegerConstantExpr(Context);
    if (array && index && !index->isNegative() &&
        index->getLimitedValue() < array->getSize().getLimitedValue()) return true;
    candidate("4.1.3", expression->getExprLoc(),
              "Subscript requires bounds, pointer validity and object lifetime proofs");
    return true;
  }
  bool VisitMemberExpr(clang::MemberExpr *expression) {
    if (expression->isArrow() &&
        !clang::isa<clang::CXXThisExpr>(expression->getBase()->IgnoreParenImpCasts()) &&
        !baseline_library::freshDirectoryEntryMember(expression, Context))
      candidate("4.1.3", expression->getExprLoc(),
                "Pointer member access requires a valid live object proof");
    return true;
  }
  bool VisitCXXDeleteExpr(clang::CXXDeleteExpr *expression) {
    candidate("4.1.3", expression->getExprLoc(),
              "Deletion requires live allocation, matching allocation form and valid dynamic-type proofs");
    return true;
  }
  bool VisitCallExpr(clang::CallExpr *expression) {
    const auto *callee = expression->getDirectCallee();
    if (!callee || !inStandardNamespace(callee->getDeclContext())) return true;
    // Algorithm predicate/comparator overloads, including their parallel forms.
    // Ordinary constructors, printing and atomic operations are not predicates.
    const bool algorithm = llvm::StringSwitch<bool>(callee->getNameAsString())
        .Cases("all_of", "any_of", "none_of", "find_if", "find_if_not", true)
        .Cases("count_if", "adjacent_find", "mismatch", "equal", "search", true)
        .Cases("search_n", "find_end", "find_first_of", "copy_if", true)
        .Cases("remove_if", "remove_copy_if", "replace_if", "replace_copy_if", true)
        .Cases("unique", "unique_copy", "partition", "stable_partition", true)
        .Cases("is_partitioned", "partition_copy", "partition_point", true)
        .Cases("sort", "stable_sort", "partial_sort", "partial_sort_copy", true)
        .Cases("is_sorted", "is_sorted_until", "nth_element", "lower_bound", true)
        .Cases("upper_bound", "equal_range", "binary_search", "merge", true)
        .Cases("inplace_merge", "includes", "set_difference", "set_intersection", true)
        .Cases("set_symmetric_difference", "set_union", "is_heap", "is_heap_until", true)
        .Cases("make_heap", "push_heap", "pop_heap", "sort_heap", true)
        .Cases("min", "max", "minmax", "min_element", "max_element", true)
        .Cases("minmax_element", "lexicographical_compare", "next_permutation", true)
        .Cases("prev_permutation", "is_permutation", "clamp", true).Default(false);
    if (!algorithm) return true;
    for (const auto *argument : expression->arguments()) {
      if (!mayBeCallable(argument->getType()) || PureExpressions(Context).callback(argument)) continue;
      candidate("28.3.1", expression->getExprLoc(),
                "Standard algorithm callable may have persistent predicate effects; callback body requires review");
      break;
    }
    return true;
  }

 private:
  clang::ASTContext &Context;
  const llvm::DenseSet<const clang::FunctionDecl *> &Used;
  const baseline_context::Proof &Contexts;
  bool InStaticAssert = false;
  bool InUnevaluated = false;
};
}  // namespace

void collectSemanticBaselineFacts(clang::ASTContext &context) {
  FunctionUseCollector uses(context);
  uses.TraverseDecl(context.getTranslationUnitDecl());
  uses.resolveReachability();
  baseline_context::Proof contexts(context);
  SemanticBaselineVisitor visitor(context, uses.Used, contexts);
  const auto start = context.getSourceManager().getLocForStartOfFile(
      context.getSourceManager().getMainFileID());
  visitor.candidate("4.1.1", start,
      "Coverage obligation: successful parsing does not establish complete C++17 conformance, including linking and implementation requirements", true);
  if (!context.getLangOpts().CPlusPlus17 || context.getLangOpts().CPlusPlus20)
    visitor.candidate("4.1.1", start,
        "Coverage obligation: extraction language mode is not C++17; accepted syntax and semantics require separate C++17 validation", true);
  visitor.candidate("4.1.3", start,
      "Coverage obligation: absence of undefined or critical unspecified behavior is not proven by this baseline", true);
  visitor.candidate("0.1.1", start,
      "Coverage obligation: CFG liveness covers explicit local scalar stores; aliases, aggregates, implicit stores, library effects and assembly require additional analysis", true);
  visitor.candidate("0.2.4", start,
      "Coverage obligation: complete-program function visibility and reachability, including external callbacks, are not proven", true);
  visitor.candidate("6.2.3", start,
      "Coverage obligation: unique-source analysis compares available definition identities; unavailable translation units and unmodelled entities remain unproven", true);
  visitor.candidate("18.5.1", start,
      "Coverage obligation: dependent exception specifications and implicit noexcept functions require exception-propagation analysis", true);
  visitor.candidate("28.3.1", start,
      "Coverage obligation: listed standard algorithm predicates are reviewed; type-erased, indirect, implicit and unrecognized library predicate invocations require additional effect analysis", true);
  visitor.TraverseDecl(context.getTranslationUnitDecl());
}
