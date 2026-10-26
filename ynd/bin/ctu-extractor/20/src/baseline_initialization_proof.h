#pragma once

#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

// Prove defined scalar values, not absence of arithmetic UB or valid object
// lifetimes. Only internal globals whose complete write/escape set is visible
// may enter through the public API. All unknown return values remain unknown.
namespace baseline_initialization {
inline const clang::VarDecl *variable(const clang::Expr *e) {
  if (!e) return nullptr;
  if (const auto *r = clang::dyn_cast<clang::DeclRefExpr>(e->IgnoreParenImpCasts()))
    if (const auto *v = clang::dyn_cast<clang::VarDecl>(r->getDecl())) return v->getCanonicalDecl();
  return nullptr;
}
struct Uses {
  bool Unknown = false;
  llvm::SmallVector<const clang::Expr *, 2> Writes;
};
class Index : public clang::RecursiveASTVisitor<Index> {
 public:
  explicit Index(clang::ASTContext &context) : Context(context) {}
  clang::ASTContext &Context;
  llvm::DenseMap<const clang::VarDecl *, Uses> Variables;
  bool Assembly = false;
  bool shouldVisitTemplateInstantiations() const { return true; }
  bool shouldVisitImplicitCode() const { return true; }
  void escape(const clang::Expr *e) {
    // Reference casts preserve the aliased storage. Do not unwrap value casts:
    // returning/copying a converted value does not publish its source object.
    while (e) {
      e = e->IgnoreParenImpCasts();
      const auto *cast = clang::dyn_cast<clang::ExplicitCastExpr>(e);
      if (!cast || !cast->isGLValue()) break;
      e = cast->getSubExpr();
    }
    if (const auto *conditional = clang::dyn_cast_or_null<clang::ConditionalOperator>(e)) {
      escape(conditional->getTrueExpr());
      escape(conditional->getFalseExpr());
      return;
    }
    if (const auto *comma = clang::dyn_cast_or_null<clang::BinaryOperator>(e)) {
      if (comma->getOpcode() == clang::BO_Comma) { escape(comma->getRHS()); return; }
    }
    if (auto *v = variable(e)) Variables[v].Unknown = true;
  }
  bool VisitExplicitCastExpr(clang::ExplicitCastExpr *e) {
    if (e->isGLValue()) escape(e);
    return true;
  }
  bool VisitBinaryOperator(clang::BinaryOperator *e) {
    if (e->isAssignmentOp()) {
      if (auto *v = variable(e->getLHS())) {
        if (e->isCompoundAssignmentOp()) Variables[v].Unknown = true;
        else Variables[v].Writes.push_back(e->getRHS());
      } else {
        // A conditional/comma/reference-cast lvalue may select internal
        // storage without exposing its address as a separate AST node.
        escape(e->getLHS());
      }
    }
    return true;
  }
  bool VisitUnaryOperator(clang::UnaryOperator *e) {
    if (e->getOpcode() == clang::UO_AddrOf || e->isIncrementDecrementOp()) escape(e->getSubExpr());
    return true;
  }
  bool VisitVarDecl(clang::VarDecl *v) {
    if (v->getType()->isReferenceType()) escape(v->getInit());
    return true;
  }
  bool VisitCallExpr(clang::CallExpr *e) {
    for (const auto *a : e->arguments()) if (a->isGLValue()) escape(a);
    return true;
  }
  bool VisitReturnStmt(clang::ReturnStmt *s) {
    if (const auto *result = s->getRetValue()) if (result->isGLValue()) escape(result);
    return true;
  }
  bool VisitInitListExpr(clang::InitListExpr *e) {
    // Aggregate reference members bind directly, with no constructor call.
    // Value elements carry lvalue-to-rvalue conversion and are not GLValues.
    for (const auto *element : e->inits()) if (element && element->isGLValue()) escape(element);
    return true;
  }
  bool VisitCXXConstructExpr(clang::CXXConstructExpr *e) {
    for (const auto *argument : e->arguments()) if (argument->isGLValue()) escape(argument);
    return true;
  }
  bool VisitLambdaExpr(clang::LambdaExpr *e) {
    for (const auto &c : e->captures()) if (c.capturesVariable() && c.getCaptureKind() == clang::LCK_ByRef)
      if (const auto *v = clang::dyn_cast<clang::VarDecl>(c.getCapturedVar())) Variables[v->getCanonicalDecl()].Unknown = true;
    return true;
  }
  bool VisitCXXPseudoDestructorExpr(clang::CXXPseudoDestructorExpr *e) { escape(e->getBase()); return true; }
  bool VisitAsmStmt(clang::AsmStmt *statement) {
    // Trusted system-header implementation/ABI code cannot name an internal
    // user global. Passed aliases are rejected independently above. User asm
    // remains opaque even when it has no explicit operand naming the global.
    if (!Context.getSourceManager().isInSystemHeader(statement->getAsmLoc())) Assembly = true;
    return true;
  }
};
class Returns : public clang::RecursiveASTVisitor<Returns> {
 public:
  llvm::SmallVector<const clang::Expr *, 4> Values;
  bool TraverseLambdaExpr(clang::LambdaExpr *) { return true; }
  bool TraverseCXXRecordDecl(clang::CXXRecordDecl *) { return true; }
  bool VisitReturnStmt(clang::ReturnStmt *s) { Values.push_back(s->getRetValue()); return true; }
};
class Proof {
 public:
  explicit Proof(clang::ASTContext &context) : C(context), Inputs(context) {}
  bool initialized(const clang::Expr *e) {
    const auto *v = variable(e);
    if (!v || !v->hasGlobalStorage() || v->getFormalLinkage() != clang::Linkage::Internal ||
        !v->getType()->isScalarType() || v->getType().isVolatileQualified()) return false;
    if (!Indexed) { Inputs.TraverseDecl(C.getTranslationUnitDecl()); Indexed = true; }
    if (Inputs.Assembly) return false;
    return definedVariable(v, 0);
  }
  // Expression entry point for caller-owned initializer analysis. This proves
  // only the returned scalar value; parameters and arbitrary reference results
  // remain unsupported, just as for the internal-global entry point.
  bool initializedValue(const clang::Expr *e) {
    if (!Indexed) { Inputs.TraverseDecl(C.getTranslationUnitDecl()); Indexed = true; }
    // Opaque assembly prevents storage-based proofs, but cannot make a
    // literal or a trusted syscall return indeterminate. Keep that restriction
    // in definedVariable so visible storage-independent wrappers still work.
    return value(e, 0);
  }
 private:
  clang::ASTContext &C;
  bool Indexed = false;
  Index Inputs;
  llvm::DenseMap<const clang::VarDecl *, bool> Cached;
  llvm::DenseSet<const clang::VarDecl *> Active;
  llvm::DenseSet<const clang::FunctionDecl *> Functions;
  bool definedVariable(const clang::VarDecl *v, unsigned depth) {
    if (Inputs.Assembly) return false;
    v = v->getCanonicalDecl();
    if (depth > 24 || !v->getType()->isScalarType() || v->getType()->isReferenceType() ||
        v->getType().isVolatileQualified() || clang::isa<clang::ParmVarDecl>(v)) return false;
    if (auto it = Cached.find(v); it != Cached.end()) return it->second;
    if (!Active.insert(v).second) return false;
    const auto *definition = v->getDefinition();
    if (definition) v = definition;
    const auto &uses = Inputs.Variables[v->getCanonicalDecl()];
    bool good = !uses.Unknown;
    if (v->hasGlobalStorage()) {
      good &= v->getFormalLinkage() == clang::Linkage::Internal && !v->hasExternalStorage();
      if (v->hasInit()) good &= value(v->getInit(), depth + 1);
    } else {
      // No path-sensitive assignment proof: locals need an initializer and no
      // subsequent write, so a read before an assignment cannot look defined.
      good &= v->hasInit() && uses.Writes.empty();
      if (good) good = value(v->getInit(), depth + 1);
    }
    for (const auto *write : uses.Writes) if (good) good = value(write, depth + 1);
    Active.erase(v->getCanonicalDecl());
    Cached[v->getCanonicalDecl()] = good;
    return good;
  }
  bool value(const clang::Expr *e, unsigned depth) {
    if (!e || depth > 24 || e->isValueDependent() || e->getType().isNull()) return false;
    e = e->IgnoreParens();
    if (const auto *clean = clang::dyn_cast<clang::ExprWithCleanups>(e)) return value(clean->getSubExpr(), depth + 1);
    if (const auto *cast = clang::dyn_cast<clang::CastExpr>(e)) {
      if (cast->getCastKind() == clang::CK_ArrayToPointerDecay || cast->getCastKind() == clang::CK_FunctionToPointerDecay) return true;
      return value(cast->getSubExpr(), depth + 1);
    }
    if (clang::isa<clang::IntegerLiteral>(e) || clang::isa<clang::FloatingLiteral>(e) ||
        clang::isa<clang::CharacterLiteral>(e) || clang::isa<clang::CXXBoolLiteralExpr>(e) ||
        clang::isa<clang::CXXNullPtrLiteralExpr>(e) || clang::isa<clang::CXXScalarValueInitExpr>(e)) return true;
    if (const auto *v = variable(e)) return definedVariable(v, depth + 1);
    if (const auto *u = clang::dyn_cast<clang::UnaryOperator>(e)) {
      if (u->getOpcode() == clang::UO_AddrOf || u->getOpcode() == clang::UO_Deref || u->isIncrementDecrementOp()) return false;
      return value(u->getSubExpr(), depth + 1);
    }
    if (const auto *b = clang::dyn_cast<clang::BinaryOperator>(e))
      return !b->isAssignmentOp() && value(b->getLHS(), depth + 1) && value(b->getRHS(), depth + 1);
    if (const auto *q = clang::dyn_cast<clang::ConditionalOperator>(e))
      return value(q->getCond(), depth + 1) && value(q->getTrueExpr(), depth + 1) && value(q->getFalseExpr(), depth + 1);
    const auto *call = clang::dyn_cast<clang::CallExpr>(e);
    if (!call) return false;
    const auto *callee = call->getDirectCallee();
    if (!callee || !callee->getReturnType()->isScalarType()) return false;
    if (callee->isExternC() && C.getSourceManager().isInSystemHeader(callee->getLocation()) &&
        (callee->getName() == "syscall" || callee->getName() == "getpid" || callee->getName() == "gettid")) return true;
    if (const auto *method = clang::dyn_cast<clang::CXXMethodDecl>(callee)) if (method->isVirtual()) return false;
    const clang::FunctionDecl *definition = nullptr;
    if (!callee->hasBody(definition) || !Functions.insert(definition).second) return false;
    const auto *body = clang::dyn_cast<clang::CompoundStmt>(definition->getBody());
    // A final unconditional return excludes unmodeled scalar fallthrough.
    bool good = body && !body->body_empty() && clang::isa<clang::ReturnStmt>(body->body_back());
    Returns returns;
    if (good) returns.TraverseStmt(const_cast<clang::Stmt *>(definition->getBody()));
    good &= !returns.Values.empty();
    for (const auto *result : returns.Values) if (good) good = value(result, depth + 1);
    Functions.erase(definition);
    return good;
  }
};
} // namespace baseline_initialization
