#pragma once

#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"

// A scoped stream handle remains owned until fclose/freopen. This proves only
// library invalidation, not successful fopen, buffer bounds or initialization.
namespace baseline_library {
inline const clang::VarDecl *variable(const clang::Expr *e) {
  if (e) if (auto *r = clang::dyn_cast<clang::DeclRefExpr>(e->IgnoreParenImpCasts()))
    return clang::dyn_cast<clang::VarDecl>(r->getDecl());
  return nullptr;
}
inline bool systemFunction(const clang::FunctionDecl *f, clang::ASTContext &c) {
  return f && f->getIdentifier() && f->isExternC() && c.getSourceManager().isInSystemHeader(f->getLocation());
}
inline bool stdNamespace(const clang::DeclContext *d) {
  for (; d; d = d->getParent()) if (auto *n = clang::dyn_cast<clang::NamespaceDecl>(d)) if (n->isStdNamespace()) return true;
  return false;
}
// Reject custom traits/allocators and user element types with callbacks.
inline bool standardValue(clang::QualType t, unsigned depth = 0) {
  if (t.isNull() || depth > 8) return false;
  t = t.getNonReferenceType();
  if (t->isBuiltinType() || t->isEnumeralType()) return true;
  if (t->isPointerType()) return standardValue(t->getPointeeType(), depth + 1);
  if (auto *a = clang::dyn_cast<clang::ArrayType>(t.getTypePtr())) return standardValue(a->getElementType(), depth + 1);
  const auto *r = t->getAsCXXRecordDecl();
  if (!r || !stdNamespace(r->getDeclContext())) return false;
  const auto name = r->getName();
  if (name != "basic_string" && name != "basic_string_view" && name != "char_traits" && name != "allocator") return false;
  const auto *s = clang::dyn_cast<clang::ClassTemplateSpecializationDecl>(r);
  if (!s) return false;
  // Only the library-provided plain-char traits are modeled. Other character
  // types may legally select a user specialization with callback effects.
  if (name != "allocator") {
    if (s->getTemplateArgs().size() == 0 || s->getTemplateArgs()[0].getKind() != clang::TemplateArgument::Type) return false;
    const auto *character = s->getTemplateArgs()[0].getAsType()->getAs<clang::BuiltinType>();
    if (!character || (character->getKind() != clang::BuiltinType::Char_S && character->getKind() != clang::BuiltinType::Char_U)) return false;
  }
  for (const auto &a : s->getTemplateArgs().asArray()) {
    if (a.getKind() == clang::TemplateArgument::Type) { if (!standardValue(a.getAsType(), depth + 1)) return false; }
    else if (a.getKind() != clang::TemplateArgument::Integral) return false;
  }
  return true;
}
inline const clang::Stmt *parentStmt(const clang::Stmt *s, clang::ASTContext &c) {
  auto p = c.getParents(*s);
  return p.size() == 1 ? p[0].get<clang::Stmt>() : nullptr;
}
inline const clang::CallExpr *directCallUse(const clang::Expr *e, clang::ASTContext &c) {
  const clang::Stmt *s = e;
  for (unsigned i = 0; i < 12; ++i) {
    const auto *p = parentStmt(s, c);
    if (auto *call = clang::dyn_cast_or_null<clang::CallExpr>(p)) return call;
    if (!p || (!clang::isa<clang::CastExpr>(p) && !clang::isa<clang::ParenExpr>(p))) return nullptr;
    s = p;
  }
  return nullptr;
}
inline bool handleArgument(const clang::CallExpr *call, const clang::VarDecl *v, const clang::Expr *reference, clang::ASTContext &c) {
  if (!call || !systemFunction(call->getDirectCallee(), c)) return false;
  auto name = call->getDirectCallee()->getName();
  unsigned slot = name == "fgets" ? 2 : name == "fclose" ? 0 : 99;
  return slot < call->getNumArgs() && variable(call->getArg(slot)) == v &&
      call->getArg(slot)->IgnoreParenImpCasts() == reference->IgnoreParenImpCasts();
}
class Aliases : public clang::RecursiveASTVisitor<Aliases> {
 public:
  clang::ASTContext &C; const clang::VarDecl *V; bool Safe = true; unsigned Nodes = 0;
  Aliases(clang::ASTContext &c, const clang::VarDecl *v) : C(c), V(v) {}
  bool VisitStmt(clang::Stmt *) { if (++Nodes > 20000) { Safe = false; return false; } return true; }
  bool VisitGotoStmt(clang::GotoStmt *) { Safe = false; return true; }
  bool VisitIndirectGotoStmt(clang::IndirectGotoStmt *) { Safe = false; return true; }
  bool VisitLabelStmt(clang::LabelStmt *) { Safe = false; return true; }
  bool VisitAsmStmt(clang::AsmStmt *) { Safe = false; return true; }
  bool VisitLambdaExpr(clang::LambdaExpr *e) {
    for (const auto &capture : e->captures()) if (capture.capturesVariable() && capture.getCapturedVar() == V) Safe = false;
    return true;
  }
  bool VisitDeclRefExpr(clang::DeclRefExpr *e) {
    if (e->getDecl() != V) return true;
    if (handleArgument(directCallUse(e, C), V, e, C)) return true;
    const clang::Stmt *s = e;
    for (unsigned i = 0; i < 12; ++i) {
      auto *p = parentStmt(s, C);
      if (const auto *b = clang::dyn_cast_or_null<clang::BinaryOperator>(p)) {
        const auto *other = variable(b->getLHS()) == V ? b->getRHS() : b->getLHS();
        if (b->isEqualityOp() && other->isNullPointerConstant(C, clang::Expr::NPC_NeverValueDependent)) return true;
      }
      if (!p || (!clang::isa<clang::CastExpr>(p) && !clang::isa<clang::ParenExpr>(p))) break;
      s = p;
    }
    Safe = false; return true;
  }
};
class Effects : public clang::RecursiveASTVisitor<Effects> {
 public:
  clang::ASTContext &C; bool Safe = true; unsigned Nodes = 0;
  explicit Effects(clang::ASTContext &c) : C(c) {}
  bool VisitStmt(clang::Stmt *) { if (++Nodes > 10000) { Safe = false; return false; } return true; }
  bool VisitCallExpr(clang::CallExpr *call) {
    auto *f = call->getDirectCallee();
    if (systemFunction(f, C)) {
      auto n = f->getName();
      if (n == "fgets" || n == "sscanf" || n == "__isoc99_sscanf" || n == "strrchr") return true;
    }
    // Only the actual standard value types, with no custom allocator/traits,
    // have a contract excluding hidden stream invalidation here.
    if (f && stdNamespace(f->getDeclContext()) && C.getSourceManager().isInSystemHeader(f->getLocation())) {
      const auto n = f->getNameAsString();
      bool values = true;
      for (const auto *a : call->arguments()) values &= standardValue(a->getType());
      if (const auto *m = clang::dyn_cast<clang::CXXMemberCallExpr>(call)) values &= standardValue(m->getImplicitObjectArgument()->getType());
      if (values && (n == "operator==" || n == "operator!=")) return true;
    }
    Safe = false; return true;
  }
  bool VisitCXXNewExpr(clang::CXXNewExpr *) { Safe = false; return true; }
  bool VisitCXXDeleteExpr(clang::CXXDeleteExpr *) { Safe = false; return true; }
  bool VisitCXXBindTemporaryExpr(clang::CXXBindTemporaryExpr *) { Safe = false; return true; }
  bool VisitCXXConstructExpr(clang::CXXConstructExpr *e) {
    // Even default allocators can invoke replacement allocation callbacks.
    if (!standardValue(e->getType()) || !e->getConstructor()->isTrivial()) Safe = false;
    return true;
  }
  bool VisitVarDecl(clang::VarDecl *v) {
    // Unknown implicit destructors may invalidate streams too.
    if (const auto *record = C.getBaseElementType(v->getType())->getAsCXXRecordDecl())
      if (!standardValue(C.getBaseElementType(v->getType())) || !record->hasTrivialDestructor()) Safe = false;
    return true;
  }
};
class Proof {
 public:
  explicit Proof(clang::ASTContext &c) : C(c) {}
  bool validStream(const clang::Expr *e) const {
    const auto *v = variable(e);
    if (!v || !v->hasLocalStorage() || !v->getType()->isPointerType() || !v->hasInit()) return false;
    const auto *open = clang::dyn_cast<clang::CallExpr>(v->getInit()->IgnoreParenImpCasts());
    if (!open || !systemFunction(open->getDirectCallee(), C) || open->getDirectCallee()->getName() != "fopen") return false;
    const auto *function = clang::dyn_cast<clang::FunctionDecl>(v->getDeclContext());
    if (!function || !function->hasBody()) return false;
    auto parents = C.getParents(*v);
    const auto *decl = parents.size() == 1 ? parents[0].get<clang::DeclStmt>() : nullptr;
    const auto *scope = decl ? clang::dyn_cast_or_null<clang::CompoundStmt>(parentStmt(decl, C)) : nullptr;
    if (!scope || !decl->isSingleDecl()) return false;
    const clang::Stmt *use = e;
    while (use && parentStmt(use, C) != scope) use = parentStmt(use, C);
    if (!use) return false;
    Aliases aliases(C, v); aliases.TraverseStmt(function->getBody());
    if (!aliases.Safe) return false;
    bool started = false;
    for (const auto *s : scope->body()) {
      if (s == decl) { started = true; continue; }
      if (!started) { if (s == use) return false; continue; }
      Effects effects(C); effects.TraverseStmt(const_cast<clang::Stmt *>(s));
      if (!effects.Safe) return false;
      if (s == use) return true;
    }
    return false;
  }
 private:
  clang::ASTContext &C;
};
} // namespace baseline_library
