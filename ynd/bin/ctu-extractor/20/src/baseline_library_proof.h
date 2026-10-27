#pragma once

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclTemplate.h"
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
// A braced range of string literals has compiler-owned array storage and
// static pointees; neither comes from an invalidatable library buffer. Return
// 2 for the hidden iterators, 1 for the user loop variable (whose writes and
// aliases must still be checked by the caller).
inline unsigned literalRangePointer(const clang::VarDecl *variable, clang::ASTContext &context) {
  if (!variable || variable->hasAttr<clang::CleanupAttr>() || !variable->getType()->isPointerType()) return 0;
  const auto parents = context.getParents(*variable);
  const auto *declaration = parents.size() == 1 ? parents[0].get<clang::DeclStmt>() : nullptr;
  const auto *loop = declaration ? clang::dyn_cast_or_null<clang::CXXForRangeStmt>(parentStmt(declaration, context)) : nullptr;
  if (!loop || !loop->getRangeInit()) return 0;
  const bool user = loop->getLoopVariable() == variable;
  if (!user && declaration != loop->getBeginStmt() && declaration != loop->getEndStmt()) return 0;
  const clang::Expr *value = loop->getRangeInit();
  const clang::CXXStdInitializerListExpr *standardList = nullptr;
  for (unsigned depth = 0; value && depth != 8; ++depth) {
    value = value->IgnoreParenImpCasts();
    if (const auto *list = clang::dyn_cast<clang::CXXStdInitializerListExpr>(value)) {
      standardList = list; value = list->getSubExpr();
    } else if (const auto *temporary = clang::dyn_cast<clang::MaterializeTemporaryExpr>(value)) value = temporary->getSubExpr();
    else if (const auto *cleanup = clang::dyn_cast<clang::ExprWithCleanups>(value)) value = cleanup->getSubExpr();
    else break;
  }
  const auto *list = clang::dyn_cast_or_null<clang::InitListExpr>(value);
  if (!standardList || !list || list->getNumInits() == 0) return 0;
  // Compiler recognition also accepts a user-provided initializer_list. Its
  // endpoints need not reference the literal backing array at all.
  const auto *record = clang::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
      standardList->getType()->getAsCXXRecordDecl());
  const auto *primary = record ? record->getSpecializedTemplate() : nullptr;
  if (!primary || primary->getName() != "initializer_list" ||
      !context.getSourceManager().isInSystemHeader(primary->getLocation()) ||
      !context.getSourceManager().isInSystemHeader(record->getLocation())) return 0;
  const clang::DeclContext *space = primary->getDeclContext();
  while (const auto *ns = clang::dyn_cast<clang::NamespaceDecl>(space)) {
    if (!ns->isInline()) break;
    space = ns->getParent();
  }
  const auto *standard = clang::dyn_cast<clang::NamespaceDecl>(space);
  if (!standard || standard->getName() != "std" ||
      !standard->getParent()->isTranslationUnit()) return 0;
  auto singleVariable = [](const clang::DeclStmt *statement) -> const clang::VarDecl * {
    return statement && statement->isSingleDecl()
        ? clang::dyn_cast<clang::VarDecl>(statement->getSingleDecl()) : nullptr;
  };
  const auto *range = singleVariable(loop->getRangeStmt());
  const auto *begin = singleVariable(loop->getBeginStmt());
  const auto *end = singleVariable(loop->getEndStmt());
  if (!range || !range->isImplicit() || !begin || !begin->isImplicit() ||
      !end || !end->isImplicit()) return 0;
  auto endpoint = [&](const clang::VarDecl *decl, llvm::StringRef name) {
    const auto *call = decl->hasInit()
        ? clang::dyn_cast<clang::CXXMemberCallExpr>(decl->getInit()->IgnoreParenImpCasts()) : nullptr;
    const auto *method = call ? call->getMethodDecl() : nullptr;
    const auto *object = call ? clang::dyn_cast<clang::DeclRefExpr>(
        call->getImplicitObjectArgument()->IgnoreParenImpCasts()) : nullptr;
    return method && method->getName() == name && method->isConst() &&
        !method->isStatic() && call->getNumArgs() == 0 &&
        object && object->getDecl() == range &&
        method->getParent()->getCanonicalDecl() == record->getCanonicalDecl() &&
        context.getSourceManager().isInSystemHeader(method->getLocation());
  };
  if (!endpoint(begin, "begin") || !endpoint(end, "end")) return 0;
  for (const auto *element : list->inits())
    if (!clang::isa<clang::StringLiteral>(element->IgnoreParenImpCasts())) return 0;
  return user ? 1 : 2;
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
inline bool handleArgument(const clang::CallExpr *call, const clang::VarDecl *v, const clang::Expr *reference, clang::ASTContext &c, bool directory = false) {
  if (!call || !systemFunction(call->getDirectCallee(), c)) return false;
  auto name = call->getDirectCallee()->getName();
  unsigned slot = directory ? ((name == "readdir" || name == "closedir") ? 0 : 99) : (name == "fgets" ? 2 : name == "fclose" ? 0 : 99);
  return slot < call->getNumArgs() && variable(call->getArg(slot)) == v &&
      call->getArg(slot)->IgnoreParenImpCasts() == reference->IgnoreParenImpCasts();
}
// An entry can be interior DIR storage. Until we have a buffer escape
// proof, only discarded or immediately converted-to-bool results are allowed;
// arbitrary consumers could reconstruct the owner from an interior address.
inline bool directoryEntryDiscarded(const clang::CallExpr *call, clang::ASTContext &context) {
  const clang::Stmt *node = call;
  for (unsigned depth = 0; depth != 12; ++depth) {
    const auto *p = parentStmt(node, context);
    if (!p) return false;
    if (clang::isa<clang::CompoundStmt>(p)) return true;
    if (const auto *cast = clang::dyn_cast<clang::CastExpr>(p)) {
      if (cast->getCastKind() == clang::CK_PointerToBoolean || cast->getCastKind() == clang::CK_ToVoid) return true;
      if (cast->getCastKind() != clang::CK_NoOp) return false;
    } else if (const auto *unary = clang::dyn_cast<clang::UnaryOperator>(p)) {
      return unary->getOpcode() == clang::UO_LNot;
    } else if (const auto *binary = clang::dyn_cast<clang::BinaryOperator>(p)) {
      const auto *other = binary->getLHS() == node ? binary->getRHS() : binary->getLHS();
      return binary->isEqualityOp() && other->isNullPointerConstant(context, clang::Expr::NPC_NeverValueDependent);
    } else if (!clang::isa<clang::ParenExpr, clang::ExprWithCleanups>(p)) return false;
    node = p;
  }
  return false;
}
// A copied name does not export the entry buffer, even if that buffer is
// interior DIR storage. Require the actual default string specialization:
// user traits/allocators or user specializations can retain their input.
inline bool defaultCharString(clang::QualType type, clang::ASTContext &context) {
  const auto *record = clang::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
      type.getNonReferenceType()->getAsCXXRecordDecl());
  if (!record || record->getName() != "basic_string" || !stdNamespace(record->getDeclContext()) ||
      !context.getSourceManager().isInSystemHeader(record->getLocation()) || record->getTemplateArgs().size() != 3) return false;
  const auto &args = record->getTemplateArgs();
  if (args[0].getKind() != clang::TemplateArgument::Type ||
      !context.hasSameType(args[0].getAsType(), context.CharTy)) return false;
  for (unsigned i = 1; i != 3; ++i) {
    if (args[i].getKind() != clang::TemplateArgument::Type) return false;
    const auto *part = clang::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(args[i].getAsType()->getAsCXXRecordDecl());
    if (!part || part->getName() != (i == 1 ? "char_traits" : "allocator") ||
        !stdNamespace(part->getDeclContext()) || !context.getSourceManager().isInSystemHeader(part->getLocation()) ||
        part->getTemplateArgs().size() != 1 || part->getTemplateArgs()[0].getKind() != clang::TemplateArgument::Type ||
        !context.hasSameType(part->getTemplateArgs()[0].getAsType(), context.CharTy)) return false;
  }
  return true;
}
class DirectoryEntryUses : public clang::RecursiveASTVisitor<DirectoryEntryUses> {
 public:
  clang::ASTContext &C; const clang::VarDecl *V; const clang::WhileStmt *Loop;
  bool Safe = true; unsigned Nodes = 0;
  DirectoryEntryUses(clang::ASTContext &c, const clang::VarDecl *v, const clang::WhileStmt *loop) : C(c), V(v), Loop(loop) {}
  bool VisitStmt(clang::Stmt *) { if (++Nodes > 20000) { Safe = false; return false; } return true; }
  bool VisitLambdaExpr(clang::LambdaExpr *lambda) {
    for (const auto &capture : lambda->captures()) if (capture.capturesVariable() && capture.getCapturedVar() == V) Safe = false;
    return true;
  }
  bool VisitDeclRefExpr(clang::DeclRefExpr *reference) {
    if (reference->getDecl() == V && !safeUse(reference)) Safe = false;
    return true;
  }
  bool safeUse(const clang::DeclRefExpr *reference) const {
    if (Loop->getCond()->IgnoreParenImpCasts() == reference) return true;
    const clang::Stmt *node = reference;
    for (unsigned depth = 0; depth != 8; ++depth) {
      const auto *parent = parentStmt(node, C);
      if (clang::isa_and_nonnull<clang::ParenExpr>(parent)) { node = parent; continue; }
      if (const auto *cast = clang::dyn_cast_or_null<clang::ImplicitCastExpr>(parent)) {
        if (cast->getCastKind() != clang::CK_LValueToRValue && cast->getCastKind() != clang::CK_NoOp) return false;
        node = parent; continue;
      }
      const auto *member = clang::dyn_cast_or_null<clang::MemberExpr>(parent);
      const auto *field = member ? clang::dyn_cast<clang::FieldDecl>(member->getMemberDecl()) : nullptr;
      if (!member || !member->isArrow() || member->getBase() != node || !field || field->getName() != "d_name" ||
          !C.getSourceManager().isInSystemHeader(field->getLocation()) || !field->getType()->isArrayType() ||
          !C.hasSameType(C.getBaseElementType(field->getType()), C.CharTy)) return false;
      const clang::Stmt *source = member;
      for (unsigned n = 0; n != 8; ++n) {
        const auto *use = parentStmt(source, C);
        if (clang::isa_and_nonnull<clang::ParenExpr>(use)) { source = use; continue; }
        if (const auto *cast = clang::dyn_cast_or_null<clang::ImplicitCastExpr>(use)) {
          if (cast->getCastKind() != clang::CK_ArrayToPointerDecay && cast->getCastKind() != clang::CK_NoOp) return false;
          source = use; continue;
        }
        if (const auto *subscript = clang::dyn_cast_or_null<clang::ArraySubscriptExpr>(use)) {
          const auto *array = C.getAsConstantArrayType(field->getType());
          auto index = subscript->getIdx()->getIntegerConstantExpr(C);
          if (!array || array->getSize().isZero() || subscript->getBase() != source || !index || !index->isZero()) return false;
          const auto *read = clang::dyn_cast_or_null<clang::ImplicitCastExpr>(parentStmt(subscript, C));
          return read && read->getCastKind() == clang::CK_LValueToRValue;
        }
        if (const auto *reader = clang::dyn_cast_or_null<clang::CallExpr>(use)) {
          const auto *target = reader->getDirectCallee();
          if (systemFunction(target, C) && reader->getNumArgs() > 0 && reader->getArg(0) == source &&
              ((target->getName() == "strlen" && reader->getNumArgs() == 1) ||
               (target->getName() == "strspn" && reader->getNumArgs() == 2))) return true;
        }
        const auto *call = clang::dyn_cast_or_null<clang::CXXOperatorCallExpr>(use);
        const auto *function = call ? call->getDirectCallee() : nullptr;
        if (!call || call->getOperator() != clang::OO_Plus || call->getNumArgs() != 2 || !function ||
            !stdNamespace(function->getDeclContext()) || !C.getSourceManager().isInSystemHeader(function->getLocation()) ||
            !defaultCharString(call->getType(), C)) return false;
        if (call->getArg(0) == source) return defaultCharString(call->getArg(1)->getType(), C);
        if (call->getArg(1) == source) return defaultCharString(call->getArg(0)->getType(), C);
        return false;
      }
      return false;
    }
    return false;
  }
};
inline bool directoryEntryCopied(const clang::CallExpr *call, clang::ASTContext &context) {
  const clang::Stmt *node = call;
  const clang::VarDecl *entry = nullptr;
  for (unsigned depth = 0; depth != 8; ++depth) {
    const auto parents = context.getParents(*node);
    if (parents.size() != 1) return false;
    if ((entry = parents[0].get<clang::VarDecl>())) break;
    const auto *parent = parents[0].get<clang::Stmt>();
    if (!parent || !clang::isa<clang::ImplicitCastExpr, clang::ParenExpr>(parent)) return false;
    node = parent;
  }
  if (!entry || entry->hasAttr<clang::CleanupAttr>() || !entry->hasLocalStorage() || !entry->getType()->isPointerType() || !entry->hasInit() ||
      entry->getInit()->IgnoreParenImpCasts() != call) return false;
  const auto parents = context.getParents(*entry);
  const auto *declaration = parents.size() == 1 ? parents[0].get<clang::DeclStmt>() : nullptr;
  const auto *loop = declaration ? clang::dyn_cast_or_null<clang::WhileStmt>(parentStmt(declaration, context)) : nullptr;
  if (!loop || loop->getConditionVariable() != entry) return false;
  const auto *function = clang::dyn_cast<clang::FunctionDecl>(entry->getDeclContext());
  if (!function || !function->hasBody()) return false;
  DirectoryEntryUses uses(context, entry, loop);
  return uses.TraverseStmt(function->getBody()) && uses.Safe;
}
class Aliases : public clang::RecursiveASTVisitor<Aliases> {
 public:
  clang::ASTContext &C; const clang::VarDecl *V; bool Directory; bool Safe = true; unsigned Nodes = 0;
  Aliases(clang::ASTContext &c, const clang::VarDecl *v, bool directory = false) : C(c), V(v), Directory(directory) {}
  bool VisitStmt(clang::Stmt *) { if (++Nodes > 20000) { Safe = false; return false; } return true; }
  bool VisitGotoStmt(clang::GotoStmt *) { Safe = false; return true; }
  bool VisitIndirectGotoStmt(clang::IndirectGotoStmt *) { Safe = false; return true; }
  bool VisitLabelStmt(clang::LabelStmt *) { Safe = false; return true; }
  bool VisitAsmStmt(clang::AsmStmt *) { Safe = false; return true; }
  bool VisitLambdaExpr(clang::LambdaExpr *e) {
    for (const auto &capture : e->captures()) if (capture.capturesVariable() && capture.getCapturedVar() == V) Safe = false;
    return true;
  }
  bool VisitCallExpr(clang::CallExpr *call) {
    const auto *function = call->getDirectCallee();
    if (Directory && function && function->getIdentifier() && function->getName() == "readdir" &&
        call->getNumArgs() == 1 && variable(call->getArg(0)) == V && !directoryEntryDiscarded(call, C) && !directoryEntryCopied(call, C)) Safe = false;
    return true;
  }
  bool VisitDeclRefExpr(clang::DeclRefExpr *e) {
    if (e->getDecl() != V) return true;
    if (handleArgument(directCallUse(e, C), V, e, C, Directory)) return true;
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
// With no exported DIR handle, unrelated calls cannot close its storage.
// readdir may invalidate its returned entry buffer, which this proof does not
// cover. Unlike FILE streams, DIR handles have no global close-all operation.
class DirectoryClosures : public clang::RecursiveASTVisitor<DirectoryClosures> {
 public:
  const clang::VarDecl *V; const clang::CallExpr *Allowed; bool Safe = true; unsigned Nodes = 0;
  DirectoryClosures(const clang::VarDecl *v, const clang::CallExpr *allowed) : V(v), Allowed(allowed) {}
  bool VisitStmt(clang::Stmt *) { if (++Nodes > 20000) { Safe = false; return false; } return true; }
  bool VisitCallExpr(clang::CallExpr *call) {
    const auto *function = call->getDirectCallee();
    if (function && function->getIdentifier() && function->getName() == "closedir" && call->getNumArgs() == 1 &&
        variable(call->getArg(0)) == V && call != Allowed) Safe = false;
    return true;
  }
};
// Condition-variable initializers cross a VarDecl parent before reaching
// their DeclStmt and loop. Do not lose the actual enclosing control flow at
// that declaration boundary, and do not cross arbitrary declaration kinds.
inline const clang::Stmt *directoryUseParent(const clang::Stmt *statement, clang::ASTContext &context) {
  const auto parents = context.getParents(*statement);
  if (parents.size() != 1) return nullptr;
  if (const auto *parent = parents[0].get<clang::Stmt>()) return parent;
  const auto *variable = parents[0].get<clang::VarDecl>();
  if (!variable || !variable->hasLocalStorage() || !variable->hasInit()) return nullptr;
  const auto declarationParents = context.getParents(*variable);
  return declarationParents.size() == 1 ? declarationParents[0].get<clang::DeclStmt>() : nullptr;
}
class Proof {
 public:
  explicit Proof(clang::ASTContext &c) : C(c) {}
  bool validDirectory(const clang::Expr *expression) const {
    const auto *v = variable(expression);
    if (!v || v->hasAttr<clang::CleanupAttr>() || !v->hasLocalStorage() || !v->getType()->isPointerType() || !v->hasInit()) return false;
    const auto *open = clang::dyn_cast<clang::CallExpr>(v->getInit()->IgnoreParenImpCasts());
    if (!open || !systemFunction(open->getDirectCallee(), C) || open->getDirectCallee()->getName() != "opendir") return false;
    const auto *function = clang::dyn_cast<clang::FunctionDecl>(v->getDeclContext());
    if (!function || !function->hasBody()) return false;
    const auto parents = C.getParents(*v);
    const auto *declaration = parents.size() == 1 ? parents[0].get<clang::DeclStmt>() : nullptr;
    const auto *scope = declaration ? clang::dyn_cast_or_null<clang::CompoundStmt>(parentStmt(declaration, C)) : nullptr;
    if (!scope || !declaration->isSingleDecl()) return false;
    Aliases aliases(C, v, true);
    if (!aliases.TraverseStmt(function->getBody()) || !aliases.Safe) return false;
    const auto *call = directCallUse(expression, C);
    const auto *callee = call ? call->getDirectCallee() : nullptr;
    const bool consuming = callee && callee->getIdentifier() && callee->getName() == "closedir";
    const clang::Stmt *use = expression;
    for (unsigned depth = 0; use && directoryUseParent(use, C) != scope && depth != 64; ++depth) {
      // An owner declared outside a loop must not be consumed repeatedly.
      if (consuming && clang::isa<clang::ForStmt, clang::CXXForRangeStmt, clang::WhileStmt, clang::DoStmt>(use)) return false;
      use = directoryUseParent(use, C);
    }
    if (!use || directoryUseParent(use, C) != scope || use == declaration) return false;
    if (consuming && clang::isa<clang::ForStmt, clang::CXXForRangeStmt, clang::WhileStmt, clang::DoStmt>(use)) return false;
    DirectoryClosures closures(v, consuming ? call : nullptr);
    bool started = false;
    for (const auto *statement : scope->body()) {
      if (statement == declaration) { started = true; continue; }
      if (!started) continue;
      if (!closures.TraverseStmt(const_cast<clang::Stmt *>(statement)) || !closures.Safe) return false;
      if (statement == use) return true;
    }
    return false;
  }
  bool validStream(const clang::Expr *e) const {
    const auto *v = variable(e);
    if (!v || v->hasAttr<clang::CleanupAttr>() || !v->hasLocalStorage() || !v->getType()->isPointerType() || !v->hasInit()) return false;
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
// The first condition after a successful readdir has no intervening epoch
// change. Keep its expression language small enough to exclude callbacks and
// another readdir/closedir even in unspecified argument evaluation order.
inline bool directoryFirstCondition(const clang::Expr *expression, const clang::VarDecl *entry,
                                    clang::ASTContext &context, unsigned depth = 0) {
  if (!expression || depth > 24) return false;
  expression = expression->IgnoreParenImpCasts();
  if (clang::isa<clang::IntegerLiteral, clang::CharacterLiteral, clang::StringLiteral, clang::CXXBoolLiteralExpr>(expression)) return true;
  if (const auto *binary = clang::dyn_cast<clang::BinaryOperator>(expression))
    return (binary->isComparisonOp() || binary->isLogicalOp()) &&
        directoryFirstCondition(binary->getLHS(), entry, context, depth + 1) &&
        directoryFirstCondition(binary->getRHS(), entry, context, depth + 1);
  if (const auto *member = clang::dyn_cast<clang::MemberExpr>(expression)) {
    const auto *field = clang::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    return member->isArrow() && variable(member->getBase()) == entry && field && field->getName() == "d_name" &&
        context.getSourceManager().isInSystemHeader(field->getLocation()) && field->getType()->isArrayType() &&
        context.hasSameType(context.getBaseElementType(field->getType()), context.CharTy);
  }
  if (const auto *subscript = clang::dyn_cast<clang::ArraySubscriptExpr>(expression)) {
    const auto *base = subscript->getBase()->IgnoreParenImpCasts();
    const auto *array = context.getAsConstantArrayType(base->getType());
    auto index = subscript->getIdx()->getIntegerConstantExpr(context);
    return array && !array->getSize().isZero() && index && index->isZero() &&
        directoryFirstCondition(base, entry, context, depth + 1);
  }
  if (const auto *call = clang::dyn_cast<clang::CallExpr>(expression)) {
    const auto *function = call->getDirectCallee();
    if (!systemFunction(function, context) ||
        !((function->getName() == "strlen" && call->getNumArgs() == 1) ||
          (function->getName() == "strspn" && call->getNumArgs() == 2))) return false;
    for (const auto *argument : call->arguments())
      if (!directoryFirstCondition(argument, entry, context, depth + 1)) return false;
    return true;
  }
  return false;
}
// With no exported owner or entry alias, unrelated callbacks cannot reach
// this buffer. Only another read on the same stream changes its epoch; reads
// of a different stream do not. Closures are checked by validDirectory.
class DirectoryEpoch : public clang::RecursiveASTVisitor<DirectoryEpoch> {
 public:
  const clang::VarDecl *Owner; bool Stable = true; unsigned Nodes = 0;
  explicit DirectoryEpoch(const clang::VarDecl *owner) : Owner(owner) {}
  bool VisitStmt(clang::Stmt *) { if (++Nodes > 20000) { Stable = false; return false; } return true; }
  bool VisitCallExpr(clang::CallExpr *call) {
    const auto *function = call->getDirectCallee();
    if (function && function->getIdentifier() && function->getName() == "readdir" &&
        call->getNumArgs() == 1 && variable(call->getArg(0)) == Owner) Stable = false;
    return true;
  }
};
inline bool freshDirectoryEntryMember(const clang::MemberExpr *member, clang::ASTContext &context) {
  const auto *entry = member->isArrow() ? variable(member->getBase()) : nullptr;
  if (!entry || entry->hasAttr<clang::CleanupAttr>() || entry->getType().isVolatileQualified() || !entry->hasInit()) return false;
  const auto *call = clang::dyn_cast<clang::CallExpr>(entry->getInit()->IgnoreParenImpCasts());
  if (!call || call->getNumArgs() != 1 || !systemFunction(call->getDirectCallee(), context) ||
      call->getDirectCallee()->getName() != "readdir" || !directoryEntryCopied(call, context)) return false;
  const auto *owner = variable(call->getArg(0));
  if (!owner || owner->hasAttr<clang::CleanupAttr>() || owner->getType().isVolatileQualified()) return false;
  const auto parents = context.getParents(*entry);
  const auto *declaration = parents.size() == 1 ? parents[0].get<clang::DeclStmt>() : nullptr;
  const auto *loop = declaration ? clang::dyn_cast_or_null<clang::WhileStmt>(parentStmt(declaration, context)) : nullptr;
  const auto *body = loop ? clang::dyn_cast<clang::CompoundStmt>(loop->getBody()) : nullptr;
  if (!body) return false;
  const auto *first = !body->body_empty() ? clang::dyn_cast<clang::IfStmt>(*body->body_begin()) : nullptr;
  const clang::Stmt *node = member;
  bool inCondition = false, inBody = false;
  for (unsigned depth = 0; node && depth != 64; ++depth) {
    if (first && node == first->getCond()) inCondition = true;
    if (node == body) { inBody = true; break; }
    node = directoryUseParent(node, context);
  }
  if (!inBody) return false;
  const bool firstCondition = inCondition && first && !first->getInit() && !first->getConditionVariable() &&
      directoryFirstCondition(first->getCond(), entry, context);
  DirectoryEpoch epoch(owner);
  const bool stableEpoch = epoch.TraverseStmt(const_cast<clang::CompoundStmt *>(body)) && epoch.Stable;
  if (!firstCondition && !stableEpoch) return false;
  const auto *scope = clang::dyn_cast_or_null<clang::CompoundStmt>(parentStmt(loop, context));
  if (!scope) return false;
  bool nonnull = false;
  for (const auto *statement : scope->body()) {
    if (statement == loop) break;
    const auto *guard = clang::dyn_cast<clang::IfStmt>(statement);
    const auto *comparison = guard ? clang::dyn_cast<clang::BinaryOperator>(guard->getCond()->IgnoreParenImpCasts()) : nullptr;
    if (!comparison || comparison->getOpcode() != clang::BO_EQ || guard->getInit() || guard->getConditionVariable()) continue;
    const auto *other = variable(comparison->getLHS()) == owner ? comparison->getRHS() :
        variable(comparison->getRHS()) == owner ? comparison->getLHS() : nullptr;
    if (!other || !other->isNullPointerConstant(context, clang::Expr::NPC_NeverValueDependent) || other->HasSideEffects(context)) continue;
    const clang::Stmt *exit = guard->getThen();
    if (const auto *block = clang::dyn_cast<clang::CompoundStmt>(exit)) exit = block->body_empty() ? nullptr : block->body_back();
    if (exit && clang::isa<clang::ReturnStmt, clang::ContinueStmt>(exit)) nonnull = true;
  }
  return nonnull && Proof(context).validDirectory(call->getArg(0));
}
} // namespace baseline_library
