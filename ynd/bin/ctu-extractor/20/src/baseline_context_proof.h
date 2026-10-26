#pragma once

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include <cstdint>

// Concrete execution scaffold, conditional on entry into main. This is not a
// proof that unavailable translation-unit startup code permits reaching main.
// No externally visible function or top-valued parameter is an assumed root.
namespace baseline_context {
class Proof {
 public:
  explicit Proof(clang::ASTContext &context) : Context(context) {
    // Library translation units have no entry context to interpret. Avoid an
    // extra full traversal of their instantiated standard-library declarations.
    bool hasMain = false;
    for (const auto *decl : Context.getTranslationUnitDecl()->lookup(&Context.Idents.get("main")))
      if (const auto *function = clang::dyn_cast<clang::FunctionDecl>(decl))
        hasMain |= function->doesThisDeclarationHaveABody();
    if (!hasMain) return;
    Roots roots(Context);
    roots.TraverseDecl(Context.getTranslationUnitDecl());
    if (roots.UnsafeStartup || !roots.Main || roots.Main->isNoReturn() || roots.Main->getNumParams()) return;
    Frame frame;
    Active.insert(roots.Main->getCanonicalDecl());
    const auto outcome = statement(roots.Main->getBody(), frame, 0);
    if (outcome == Outcome::Returned) Proven = Observed;
  }
  bool varies(const clang::Expr *condition) const {
    auto found = Proven.find(condition);
    return found != Proven.end() && found->second == 3;
  }
 private:
  enum class Outcome { Normal, Returned, Failed };
  struct Frame {
    llvm::DenseMap<const clang::VarDecl *, int64_t> Values;
    int64_t Result = 0;
  };
  class Roots : public clang::RecursiveASTVisitor<Roots> {
   public:
    explicit Roots(clang::ASTContext &context) : Context(context) {}
    bool shouldVisitTemplateInstantiations() const { return true; }
    const clang::FunctionDecl *Main = nullptr;
    bool UnsafeStartup = false;
    bool VisitFunctionDecl(clang::FunctionDecl *function) {
      if (function->isMain() && function->doesThisDeclarationHaveABody()) Main = function;
      if (function->hasAttr<clang::ConstructorAttr>() || function->hasAttr<clang::DestructorAttr>()) UnsafeStartup = true;
      return true;
    }
    bool VisitVarDecl(clang::VarDecl *variable) {
      if (!variable->isFileVarDecl() || Context.getSourceManager().isInSystemHeader(variable->getLocation())) return true;
      if (variable->hasInit() && !variable->hasConstantInitialization()) UnsafeStartup = true;
      if (const auto *record = variable->getType()->getAsCXXRecordDecl())
        if (!record->hasTrivialDestructor()) UnsafeStartup = true;
      return true;
    }
   private:
    clang::ASTContext &Context;
  };
  static bool scalar(clang::QualType type) {
    return !type.isNull() && !type.isVolatileQualified() &&
        (type->isBooleanType() || type->isSpecificBuiltinType(clang::BuiltinType::Int));
  }
  bool spend(unsigned depth) { return depth <= 24 && Steps++ < 2048; }
  bool expression(const clang::Expr *expr, Frame &frame, int64_t &value, unsigned depth) {
    if (!expr || !spend(depth)) return false;
    expr = expr->IgnoreParens();
    if (const auto *cast = clang::dyn_cast<clang::ImplicitCastExpr>(expr)) {
      if (!scalar(cast->getType()) || !scalar(cast->getSubExpr()->getType()) ||
          !expression(cast->getSubExpr(), frame, value, depth + 1)) return false;
      if (cast->getType()->isBooleanType()) value = value != 0;
      return true;
    }
    if (const auto *literal = clang::dyn_cast<clang::IntegerLiteral>(expr)) {
      if (!scalar(expr->getType()) || literal->getValue().getActiveBits() > 31) return false;
      value = literal->getValue().getZExtValue(); return true;
    }
    if (const auto *literal = clang::dyn_cast<clang::CXXBoolLiteralExpr>(expr)) {
      value = literal->getValue(); return true;
    }
    if (const auto *reference = clang::dyn_cast<clang::DeclRefExpr>(expr)) {
      const auto *variable = clang::dyn_cast<clang::VarDecl>(reference->getDecl());
      if (!variable || !scalar(variable->getType())) return false;
      auto found = frame.Values.find(variable);
      if (found == frame.Values.end()) return false;
      value = found->second; return true;
    }
    if (const auto *unary = clang::dyn_cast<clang::UnaryOperator>(expr)) {
      if (unary->getOpcode() != clang::UO_LNot || !expression(unary->getSubExpr(), frame, value, depth + 1)) return false;
      value = !value; return true;
    }
    if (const auto *binary = clang::dyn_cast<clang::BinaryOperator>(expr)) {
      int64_t left = 0, right = 0;
      if (!expression(binary->getLHS(), frame, left, depth + 1)) return false;
      if (binary->getOpcode() == clang::BO_LAnd && !left) { value = 0; return true; }
      if (binary->getOpcode() == clang::BO_LOr && left) { value = 1; return true; }
      if (!expression(binary->getRHS(), frame, right, depth + 1)) return false;
      switch (binary->getOpcode()) {
        case clang::BO_EQ: value = left == right; return true;
        case clang::BO_NE: value = left != right; return true;
        case clang::BO_LT: value = left < right; return true;
        case clang::BO_LE: value = left <= right; return true;
        case clang::BO_GT: value = left > right; return true;
        case clang::BO_GE: value = left >= right; return true;
        case clang::BO_LAnd: value = left && right; return true;
        case clang::BO_LOr: value = left || right; return true;
        default: return false;
      }
    }
    if (const auto *conditional = clang::dyn_cast<clang::ConditionalOperator>(expr)) {
      int64_t test = 0;
      if (!expression(conditional->getCond(), frame, test, depth + 1)) return false;
      observe(conditional->getCond(), test);
      return expression(test ? conditional->getTrueExpr() : conditional->getFalseExpr(), frame, value, depth + 1);
    }
    if (const auto *call = clang::dyn_cast<clang::CallExpr>(expr)) {
      const auto *function = call->getDirectCallee();
      if (!function || clang::isa<clang::CXXMethodDecl>(function) || function->isVariadic() ||
          function->isNoReturn() || function->hasAttr<clang::WeakAttr>() ||
          call->getNumArgs() != function->getNumParams()) return false;
      function = function->getDefinition();
      if (!function || (!function->getReturnType()->isVoidType() && !scalar(function->getReturnType())) ||
          Active.contains(function->getCanonicalDecl())) return false;
      Frame callee;
      for (unsigned index = 0; index < call->getNumArgs(); ++index) {
        const auto *parameter = function->getParamDecl(index);
        int64_t argument = 0;
        if (!scalar(parameter->getType()) || !expression(call->getArg(index), frame, argument, depth + 1)) return false;
        callee.Values[parameter] = parameter->getType()->isBooleanType() ? argument != 0 : argument;
      }
      Active.insert(function->getCanonicalDecl());
      const auto outcome = statement(function->getBody(), callee, depth + 1);
      Active.erase(function->getCanonicalDecl());
      if (outcome == Outcome::Failed || (outcome != Outcome::Returned && !function->getReturnType()->isVoidType())) return false;
      value = callee.Result; return true;
    }
    return false;
  }
  void observe(const clang::Expr *condition, int64_t value) { Observed[condition] |= value ? 2 : 1; }
  Outcome statement(const clang::Stmt *stmt, Frame &frame, unsigned depth) {
    if (!stmt || !spend(depth)) return Outcome::Failed;
    if (const auto *compound = clang::dyn_cast<clang::CompoundStmt>(stmt)) {
      for (const auto *child : compound->body()) {
        const auto result = statement(child, frame, depth + 1);
        if (result != Outcome::Normal) return result;
      }
      return Outcome::Normal;
    }
    if (const auto *decls = clang::dyn_cast<clang::DeclStmt>(stmt)) {
      for (const auto *decl : decls->decls()) {
        const auto *variable = clang::dyn_cast<clang::VarDecl>(decl);
        int64_t value = 0;
        if (!variable || !variable->hasLocalStorage() || !scalar(variable->getType()) ||
            !variable->hasInit() || !expression(variable->getInit(), frame, value, depth + 1)) return Outcome::Failed;
        frame.Values[variable] = value;
      }
      return Outcome::Normal;
    }
    if (const auto *ret = clang::dyn_cast<clang::ReturnStmt>(stmt))
      return !ret->getRetValue() || expression(ret->getRetValue(), frame, frame.Result, depth + 1) ? Outcome::Returned : Outcome::Failed;
    if (const auto *branch = clang::dyn_cast<clang::IfStmt>(stmt)) {
      if (branch->getInit() || branch->getConditionVariable() || branch->isConstexpr()) return Outcome::Failed;
      int64_t value = 0;
      if (!expression(branch->getCond(), frame, value, depth + 1)) return Outcome::Failed;
      observe(branch->getCond(), value);
      const auto *selected = value ? branch->getThen() : branch->getElse();
      return selected ? statement(selected, frame, depth + 1) : Outcome::Normal;
    }
    if (const auto *expr = clang::dyn_cast<clang::Expr>(stmt)) {
      int64_t ignored = 0;
      return expression(expr, frame, ignored, depth + 1) ? Outcome::Normal : Outcome::Failed;
    }
    if (clang::isa<clang::NullStmt>(stmt)) return Outcome::Normal;
    return Outcome::Failed;
  }
  clang::ASTContext &Context;
  unsigned Steps = 0;
  llvm::DenseSet<const clang::FunctionDecl *> Active;
  llvm::DenseMap<const clang::Expr *, unsigned> Observed, Proven;
};
} // namespace baseline_context
