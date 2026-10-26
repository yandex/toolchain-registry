#pragma once

#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"

// Lexical automatic-storage lifetime, independent of initialized bytes. Scan
// the whole statement containing a use so later loop effects are not ignored.
namespace baseline_lifetime {
inline const clang::Stmt *parent(const clang::Stmt *s, clang::ASTContext &c) {
  auto p = c.getParents(*s);
  return p.size() == 1 ? p[0].get<clang::Stmt>() : nullptr;
}
inline bool shape(clang::QualType t, clang::ASTContext &c, unsigned depth = 0) {
  if (t.isNull() || t->isDependentType() || depth > 8 || t->isReferenceType() || t.isVolatileQualified()) return false;
  if (t->isScalarType()) return true;
  if (const auto *a = c.getAsConstantArrayType(t)) return shape(a->getElementType(), c, depth + 1);
  const auto *r = t->getAsCXXRecordDecl();
  if (!r || !r->hasDefinition() || !r->isTriviallyCopyable() || !r->hasTrivialDestructor() || r->getNumBases()) return false;
  // Defaulted enclosing construction can invoke a user subobject constructor.
  // Validate recursively rather than treating trivially-copyable as trivial.
  for (const auto *constructor : r->ctors())
    if (!constructor->isTrivial() && !(constructor->isDefaulted() && constructor->isDefaultConstructor())) return false;
  for (const auto *field : r->fields()) {
    if (!shape(field->getType(), c, depth + 1)) return false;
    if (field->hasInClassInitializer() && (field->getInClassInitializer()->isInstantiationDependent() || !field->getInClassInitializer()->isCXX11ConstantExpr(c))) return false;
  }
  return true;
}
inline const clang::VarDecl *owner(const clang::Expr *e, clang::ASTContext &c, unsigned depth = 0) {
  if (!e || depth > 12) return nullptr;
  e = e->IgnoreParenImpCasts();
  if (auto *r = clang::dyn_cast<clang::DeclRefExpr>(e)) return clang::dyn_cast<clang::VarDecl>(r->getDecl());
  if (auto *m = clang::dyn_cast<clang::MemberExpr>(e)) {
    auto *field = clang::dyn_cast<clang::FieldDecl>(m->getMemberDecl());
    if (!m->isArrow() && field && !field->getType()->isReferenceType() && !field->getParent()->isUnion()) return owner(m->getBase(), c, depth + 1);
  }
  if (auto *s = clang::dyn_cast<clang::ArraySubscriptExpr>(e)) {
    const auto *a = c.getAsConstantArrayType(s->getBase()->IgnoreParenImpCasts()->getType());
    clang::Expr::EvalResult index;
    if (a && !s->getIdx()->isInstantiationDependent() && s->getIdx()->EvaluateAsInt(index, c) && !index.Val.getInt().isNegative() &&
        index.Val.getInt().getLimitedValue() < a->getSize().getLimitedValue()) return owner(s->getBase(), c, depth + 1);
  }
  return nullptr;
}
inline bool discarded(const clang::Expr *e, clang::ASTContext &c) {
  const clang::Stmt *node = e;
  for (unsigned i = 0; i != 8; ++i) {
    const auto *p = parent(node, c);
    if (clang::isa_and_nonnull<clang::CompoundStmt>(p)) return true;
    if (!p || !clang::isa<clang::ExprWithCleanups, clang::ParenExpr, clang::ImplicitCastExpr>(p)) return false;
    node = p;
  }
  return false;
}
class ControlFlow : public clang::RecursiveASTVisitor<ControlFlow> {
 public:
  bool Safe = true; unsigned Count = 0;
  bool VisitStmt(clang::Stmt *) { if (++Count > 30000) { Safe = false; return false; } return true; }
  bool VisitGotoStmt(clang::GotoStmt *) { Safe = false; return true; }
  bool VisitIndirectGotoStmt(clang::IndirectGotoStmt *) { Safe = false; return true; }
  bool VisitLabelStmt(clang::LabelStmt *) { Safe = false; return true; }
  bool VisitAsmStmt(clang::AsmStmt *) { Safe = false; return true; }
};
class Uses : public clang::RecursiveASTVisitor<Uses> {
 public:
  clang::ASTContext &C; const clang::VarDecl *V; bool Safe = true; unsigned Count = 0;
  Uses(clang::ASTContext &c, const clang::VarDecl *v) : C(c), V(v) {}
  bool VisitStmt(clang::Stmt *) { if (++Count > 20000) { Safe = false; return false; } return true; }
  bool VisitAsmStmt(clang::AsmStmt *) { Safe = false; return true; }
  bool VisitLambdaExpr(clang::LambdaExpr *e) {
    for (const auto &capture : e->captures()) if (capture.capturesVariable() && capture.getCapturedVar() == V) Safe = false;
    return true;
  }
  bool VisitDeclRefExpr(clang::DeclRefExpr *e) {
    if (e->getDecl() == V && !safeUse(e)) Safe = false;
    return true;
  }
  bool safeUse(const clang::Expr *reference) const {
    const clang::Stmt *node = reference;
    bool address = false;
    for (unsigned depth = 0; depth != 24; ++depth) {
      const auto *p = parent(node, C);
      if (!p) return false;
      if (auto *cast = clang::dyn_cast<clang::ImplicitCastExpr>(p)) {
        if (cast->getCastKind() == clang::CK_LValueToRValue && !address) return true;
        if (cast->getCastKind() == clang::CK_ArrayToPointerDecay) address = true;
        else if (cast->getCastKind() != clang::CK_NoOp && cast->getCastKind() != clang::CK_BitCast) return false;
        node = p; continue;
      }
      if (clang::isa<clang::ParenExpr>(p)) { node = p; continue; }
      if (auto *member = clang::dyn_cast<clang::MemberExpr>(p)) {
        const auto *field = clang::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
        if (address || member->isArrow() || !field || field->getType()->isReferenceType() || field->getParent()->isUnion()) return false;
        node = p; continue;
      }
      if (auto *subscript = clang::dyn_cast<clang::ArraySubscriptExpr>(p)) {
        if (owner(subscript, C) != V) return false;
        address = false; node = p; continue;
      }
      if (auto *unary = clang::dyn_cast<clang::UnaryOperator>(p)) {
        if (unary->getOpcode() == clang::UO_AddrOf && !address) { address = true; node = p; continue; }
        if (unary->isIncrementDecrementOp() && !address) { node = p; continue; }
        return false;
      }
      if (auto *binary = clang::dyn_cast<clang::BinaryOperator>(p)) {
        if (!address && binary->isAssignmentOp() && binary->getLHS() == node) { node = p; continue; }
        return false;
      }
      if (auto *call = clang::dyn_cast<clang::CallExpr>(p)) {
        auto *f = call->getDirectCallee();
        if (!address || !f || !f->getIdentifier() || !f->isExternC() || !C.getSourceManager().isInSystemHeader(f->getLocation())) return false;
        const auto name = f->getName();
        const unsigned buffer = name == "read" ? 1 : name == "memset" ? 0 : 99;
        if (buffer >= call->getNumArgs() || call->getArg(buffer) != node) return false;
        // memset returns its destination; retaining that result creates an alias.
        return name != "memset" || discarded(call, C);
      }
      if (auto *construct = clang::dyn_cast<clang::CXXConstructExpr>(p)) {
        auto *constructor = construct->getConstructor();
        return !address && constructor && constructor->isTrivial() && constructor->isCopyOrMoveConstructor();
      }
      if (clang::isa<clang::UnaryExprOrTypeTraitExpr>(p)) return true;
      if (clang::isa<clang::CompoundStmt>(p)) return !address;
      return false;
    }
    return false;
  }
};
class Proof {
 public:
  explicit Proof(clang::ASTContext &context) : C(context) {}
  bool live(const clang::Expr *expression) const {
    if (!expression || expression->isInstantiationDependent() || expression->getType().isNull() || expression->getType()->isUnionType()) return false;
    const auto *v = owner(expression, C);
    if (!v || !v->hasLocalStorage() || clang::isa<clang::ParmVarDecl>(v) || !shape(v->getType(), C)) return false;
    if (v->hasInit()) {
      const auto *initial = v->getInit()->IgnoreParenImpCasts();
      if (initial->isInstantiationDependent() || initial->getType().isNull()) return false;
      if (const auto *construct = clang::dyn_cast<clang::CXXConstructExpr>(initial)) {
        const auto *constructor = construct->getConstructor();
        if (!constructor) return false;
        if (!constructor->isTrivial()) {
          if (!constructor->isDefaulted() || !constructor->isDefaultConstructor()) return false;
          for (const auto *field : constructor->getParent()->fields())
            if (field->hasInClassInitializer() && (field->getInClassInitializer()->isInstantiationDependent() || !field->getInClassInitializer()->isCXX11ConstantExpr(C))) return false;
        }
      } else if (!clang::isa<clang::InitListExpr>(initial) && !initial->isCXX11ConstantExpr(C)) return false;
    }
    const auto declarationParents = C.getParents(*v);
    if (declarationParents.size() != 1) return false;
    const auto *declaration = declarationParents[0].get<clang::DeclStmt>();
    const auto *scope = declaration ? clang::dyn_cast_or_null<clang::CompoundStmt>(parent(declaration, C)) : nullptr;
    if (!scope) return false;
    const clang::Stmt *useStatement = expression;
    for (unsigned depth = 0; useStatement && depth != 64 && parent(useStatement, C) != scope; ++depth) useStatement = parent(useStatement, C);
    if (!useStatement || parent(useStatement, C) != scope || useStatement == declaration) return false;
    // A later goto may revisit a use after a later escape. Reject such functions.
    const auto *function = clang::dyn_cast<clang::FunctionDecl>(v->getDeclContext());
    if (!function || !function->getBody()) return false;
    ControlFlow flow;
    if (!flow.TraverseStmt(const_cast<clang::Stmt *>(function->getBody())) || !flow.Safe) return false;
    // An alias exported by an earlier iteration may reach replacement storage
    // at the same address. Inspect every enclosing loop, including later uses.
    for (const clang::Stmt *ancestor = scope; ancestor; ancestor = parent(ancestor, C)) {
      if (clang::isa<clang::ForStmt, clang::CXXForRangeStmt, clang::WhileStmt, clang::DoStmt>(ancestor)) {
        Uses loopUses(C, v);
        if (!loopUses.TraverseStmt(const_cast<clang::Stmt *>(ancestor)) || !loopUses.Safe) return false;
      }
    }
    Uses uses(C, v);
    bool started = false;
    for (const auto *statement : scope->body()) {
      if (statement == declaration) started = true;
      if (!started) continue;
      if (!uses.TraverseStmt(const_cast<clang::Stmt *>(statement)) || !uses.Safe) return false;
      if (statement == useStatement) return true;
    }
    return false;
  }
 private:
  clang::ASTContext &C;
};
} // namespace baseline_lifetime
