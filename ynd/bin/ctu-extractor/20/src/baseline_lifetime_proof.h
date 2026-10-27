#pragma once

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclTemplate.h"
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
// An object's use in another local's initializer belongs to that declaration
// statement. Cross only the exact initializer edge, never an arbitrary Decl.
inline const clang::Stmt *useParent(const clang::Stmt *statement, clang::ASTContext &context) {
  const auto parents = context.getParents(*statement);
  if (parents.size() != 1) return nullptr;
  if (const auto *result = parents[0].get<clang::Stmt>()) return result;
  const auto *variable = parents[0].get<clang::VarDecl>();
  if (!variable || !variable->hasLocalStorage() || !variable->hasInit() ||
      variable->getInit() != statement) return nullptr;
  const auto declarations = context.getParents(*variable);
  return declarations.size() == 1 ? declarations[0].get<clang::DeclStmt>() : nullptr;
}
inline bool systemStandardRecord(const clang::CXXRecordDecl *record, clang::ASTContext &context) {
  if (!record || !context.getSourceManager().isInSystemHeader(record->getLocation())) return false;
  for (const auto *scope = record->getDeclContext(); scope; scope = scope->getParent())
    if (const auto *space = clang::dyn_cast<clang::NamespaceDecl>(scope))
      if (space->isStdNamespace()) return true;
  return false;
}
// Prove only the owner object's lifetime, never the validity of its elements.
// Builtin elements exclude user constructors; the exact standard allocator
// excludes callbacks that receive or retain the enclosing vector's address.
inline bool standardVector(clang::QualType type, clang::ASTContext &context) {
  if (type.isNull() || type->isReferenceType() || type.isVolatileQualified()) return false;
  const auto *record = type->getAsCXXRecordDecl();
  if (!systemStandardRecord(record, context) || record->getName() != "vector") return false;
  const auto *vector = clang::dyn_cast<clang::ClassTemplateSpecializationDecl>(record);
  if (!vector || vector->getTemplateArgs().size() != 2) return false;
  const auto &element = vector->getTemplateArgs()[0];
  const auto &allocatorArgument = vector->getTemplateArgs()[1];
  if (element.getKind() != clang::TemplateArgument::Type || allocatorArgument.getKind() != clang::TemplateArgument::Type) return false;
  const auto value = element.getAsType();
  if (!value->isBuiltinType() || value->isBooleanType() || !value->isArithmeticType()) return false;
  const auto *allocator = clang::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(allocatorArgument.getAsType()->getAsCXXRecordDecl());
  return systemStandardRecord(allocator, context) && allocator->getName() == "allocator" &&
      allocator->getTemplateArgs().size() == 1 && allocator->getTemplateArgs()[0].getKind() == clang::TemplateArgument::Type &&
      context.hasSameType(value, allocator->getTemplateArgs()[0].getAsType());
}
inline bool nativeString(clang::QualType type, clang::ASTContext &context) {
  if (type.isNull() || type.getNonReferenceType().isVolatileQualified()) return false;
  const auto *string = clang::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
      type.getNonReferenceType()->getAsCXXRecordDecl());
  if (!systemStandardRecord(string, context) || string->getName() != "basic_string" ||
      string->getTemplateArgs().size() != 3) return false;
  const auto &args = string->getTemplateArgs();
  if (args[0].getKind() != clang::TemplateArgument::Type ||
      !context.hasSameType(args[0].getAsType(), context.CharTy)) return false;
  for (unsigned i = 1; i != 3; ++i) {
    if (args[i].getKind() != clang::TemplateArgument::Type) return false;
    const auto *part = clang::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
        args[i].getAsType()->getAsCXXRecordDecl());
    if (!systemStandardRecord(part, context) || part->getName() != (i == 1 ? "char_traits" : "allocator") ||
        part->getTemplateArgs().size() != 1 || part->getTemplateArgs()[0].getKind() != clang::TemplateArgument::Type ||
        !context.hasSameType(part->getTemplateArgs()[0].getAsType(), context.CharTy)) return false;
  }
  return true;
}
// Only the native-character path owner is covered. Its default string storage
// may allocate, but allocation callbacks receive no address of the path owner.
// No certificate about that string's bytes or iterators follows from this.
inline bool standardPath(clang::QualType type, clang::ASTContext &context) {
  if (type.isNull() || type->isReferenceType() || type.isVolatileQualified()) return false;
  const auto *record = type->getAsCXXRecordDecl();
  if (!systemStandardRecord(record, context) || record->getName() != "path" ||
      !record->hasDefinition()) return false;
  record = record->getDefinition();
  bool filesystem = false;
  for (const auto *scope = record->getDeclContext(); scope; scope = scope->getParent()) {
    const auto *space = clang::dyn_cast<clang::NamespaceDecl>(scope);
    if (!space) continue;
    if (space->isStdNamespace()) break;
    if (space->getName() == "filesystem") filesystem = true;
    else if (!space->isInline()) return false;
  }
  if (!filesystem) return false;
  bool character = false, string = false;
  for (const auto *decl : record->decls()) {
    const auto *alias = clang::dyn_cast<clang::TypedefNameDecl>(decl);
    if (!alias) continue;
    if (alias->getName() == "value_type")
      character = context.hasSameType(alias->getUnderlyingType(), context.CharTy);
    if (alias->getName() == "string_type") string = nativeString(alias->getUnderlyingType(), context);
  }
  return character && string;
}
inline bool pathQuery(const clang::CXXMemberCallExpr *call, clang::ASTContext &context) {
  const auto *method = call ? call->getMethodDecl() : nullptr;
  if (!method || !method->getIdentifier() || !method->isConst() || method->isStatic() ||
      method->isVirtual() || call->getNumArgs() != 0 ||
      !context.getSourceManager().isInSystemHeader(method->getLocation()) ||
      !standardPath(context.getRecordType(method->getParent()), context)) return false;
  return (method->getName() == "empty" && method->getReturnType()->isBooleanType()) ||
      (method->getName() == "parent_path" && standardPath(method->getReturnType(), context) &&
       context.hasSameUnqualifiedType(method->getReturnType(), context.getRecordType(method->getParent())));
}
// Template specializations can have a default member initializer whose AST
// expression has not been instantiated. Do not force instantiation: copying the
// record may be valid even when using that default initializer would be ill-formed.
inline bool constantFieldInitializer(const clang::FieldDecl *field, clang::ASTContext &context) {
  if (!field->hasInClassInitializer()) return true;
  const auto *initializer = field->getInClassInitializer();
  return initializer && !initializer->isInstantiationDependent() &&
      initializer->isCXX11ConstantExpr(context);
}
inline bool shape(clang::QualType t, clang::ASTContext &c, unsigned depth = 0) {
  if (t.isNull() || t->isDependentType() || depth > 8 || t->isReferenceType() || t.isVolatileQualified()) return false;
  if (t->isScalarType() || (depth == 0 && (standardVector(t, c) || standardPath(t, c)))) return true;
  if (const auto *a = c.getAsConstantArrayType(t)) return shape(a->getElementType(), c, depth + 1);
  const auto *r = t->getAsCXXRecordDecl();
  if (!r || !r->hasDefinition() || !r->isTriviallyCopyable() || !r->hasTrivialDestructor() || r->getNumBases()) return false;
  // Defaulted enclosing construction can invoke a user subobject constructor.
  // Validate recursively rather than treating trivially-copyable as trivial.
  for (const auto *constructor : r->ctors())
    if (!constructor->isTrivial() && !(constructor->isDefaulted() && constructor->isDefaultConstructor())) return false;
  for (const auto *field : r->fields()) {
    if (!shape(field->getType(), c, depth + 1)) return false;
    if (!constantFieldInitializer(field, c)) return false;
  }
  return true;
}
inline const clang::VarDecl *storage(const clang::ValueDecl *declaration) {
  if (const auto *binding = clang::dyn_cast<clang::BindingDecl>(declaration)) {
    // Tuple-like get() customizations can bind unrelated storage. Only the
    // language's direct data-member decomposition shares the owner's lifetime.
    const auto *value = binding->getBinding();
    const auto *member = value ? clang::dyn_cast<clang::MemberExpr>(value->IgnoreParenImpCasts()) : nullptr;
    const auto *base = member ? clang::dyn_cast<clang::DeclRefExpr>(member->getBase()->IgnoreParenImpCasts()) : nullptr;
    if (!member || member->isArrow() || !clang::isa<clang::FieldDecl>(member->getMemberDecl()) ||
        !base || base->getDecl() != binding->getDecomposedDecl()) return nullptr;
    return clang::dyn_cast<clang::VarDecl>(binding->getDecomposedDecl());
  }
  return clang::dyn_cast<clang::VarDecl>(declaration);
}
// The standard arithmetic parser returns a fresh plain result record and does
// not expose its address to callbacks. Arbitrary factories can publish an NRVO
// result's address, so their aggregate returns remain unproved.
inline bool parserResult(const clang::Expr *expression, clang::ASTContext &context) {
  const auto *call = clang::dyn_cast<clang::CallExpr>(expression);
  const auto *function = call ? call->getDirectCallee() : nullptr;
  if (!function || !function->getIdentifier() || function->getName() != "from_chars" ||
      !context.getSourceManager().isInSystemHeader(function->getLocation()) || call->getNumArgs() < 3) return false;
  bool standard = false;
  for (const auto *scope = function->getDeclContext(); scope; scope = scope->getParent())
    if (const auto *space = clang::dyn_cast<clang::NamespaceDecl>(scope)) standard |= space->isStdNamespace();
  const auto *record = expression->getType()->getAsCXXRecordDecl();
  return standard && record && record->getName() == "from_chars_result" &&
      call->getArg(2)->getType()->isArithmeticType();
}
// Aggregate syntax does not imply fresh, unexposed subobject storage. A
// record-returning factory can construct directly into an aggregate element
// and publish that address through NRVO, even when all constructors are trivial.
class AggregateInitialization : public clang::RecursiveASTVisitor<AggregateInitialization> {
 public:
  clang::ASTContext &C; bool Safe = true; unsigned Count = 0;
  explicit AggregateInitialization(clang::ASTContext &context) : C(context) {}
  bool VisitStmt(clang::Stmt *) { if (++Count > 10000) { Safe = false; return false; } return true; }
  bool VisitCallExpr(clang::CallExpr *call) {
    if (call->getType().isNull() || call->isInstantiationDependent()) { Safe = false; return true; }
    if (call->getType()->isRecordType() && !parserResult(call, C) && !call->isCXX11ConstantExpr(C)) Safe = false;
    return true;
  }
};
inline const clang::VarDecl *owner(const clang::Expr *e, clang::ASTContext &c, unsigned depth = 0) {
  if (!e || depth > 12) return nullptr;
  e = e->IgnoreParenImpCasts();
  if (auto *r = clang::dyn_cast<clang::DeclRefExpr>(e)) return storage(r->getDecl());
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
  bool shouldVisitImplicitCode() const { return true; }
  bool VisitStmt(clang::Stmt *) { if (++Count > 30000) { Safe = false; return false; } return true; }
  bool VisitGotoStmt(clang::GotoStmt *) { Safe = false; return true; }
  bool VisitIndirectGotoStmt(clang::IndirectGotoStmt *) { Safe = false; return true; }
  bool VisitLabelStmt(clang::LabelStmt *) { Safe = false; return true; }
  bool VisitAsmStmt(clang::AsmStmt *) { Safe = false; return true; }
  bool VisitCoroutineBodyStmt(clang::CoroutineBodyStmt *) { Safe = false; return true; }
  bool VisitCallExpr(clang::CallExpr *call) {
    const auto *callee = call->getDirectCallee();
    if (!callee) return true;
    const auto *identifier = callee->getIdentifier();
    const auto name = identifier ? identifier->getName() : llvm::StringRef();
    if (callee->hasAttr<clang::ReturnsTwiceAttr>() || name == "setjmp" || name == "_setjmp" ||
        name == "sigsetjmp" || name == "__sigsetjmp" || name == "__builtin_setjmp") Safe = false;
    return true;
  }
};
// Only a temporary closure invoked directly at its creation site can borrow
// automatic storage here. Its body is still checked by Uses below.
class ImmediateLambdaControl : public clang::RecursiveASTVisitor<ImmediateLambdaControl> {
 public:
  bool Safe = true; unsigned Count = 0;
  bool VisitStmt(clang::Stmt *) { if (++Count > 10000) { Safe = false; return false; } return true; }
  bool VisitAsmStmt(clang::AsmStmt *) { Safe = false; return true; }
  bool VisitGotoStmt(clang::GotoStmt *) { Safe = false; return true; }
  bool VisitIndirectGotoStmt(clang::IndirectGotoStmt *) { Safe = false; return true; }
  bool VisitLabelStmt(clang::LabelStmt *) { Safe = false; return true; }
  bool VisitCoroutineBodyStmt(clang::CoroutineBodyStmt *) { Safe = false; return true; }
  bool VisitLambdaExpr(clang::LambdaExpr *) { Safe = false; return true; }
  bool VisitVarDecl(clang::VarDecl *v) { if (v->hasAttr<clang::CleanupAttr>()) Safe = false; return true; }
  bool VisitCallExpr(clang::CallExpr *call) {
    const auto *callee = call->getDirectCallee();
    if (!callee || callee->hasAttr<clang::ReturnsTwiceAttr>()) Safe = false;
    if (callee && callee->getIdentifier()) {
      const auto name = callee->getName();
      if (name == "setjmp" || name == "_setjmp" || name == "sigsetjmp" || name == "__sigsetjmp" ||
          name == "longjmp" || name == "_longjmp" || name == "siglongjmp" ||
          name.starts_with("__builtin_setjmp") || name.starts_with("__builtin_longjmp") ||
          name.starts_with("__builtin_frame_address") || name.starts_with("__builtin_return_address") ||
          name.starts_with("__builtin_apply") || name.starts_with("__builtin_next_arg")) Safe = false;
    }
    return true;
  }
};
inline bool immediateLambda(const clang::LambdaExpr *lambda, clang::ASTContext &context) {
  if (!lambda || lambda->isGenericLambda() || !lambda->getCallOperator() ||
      lambda->getCallOperator()->isExplicitObjectMemberFunction()) return false;
  for (const auto &capture : lambda->captures()) {
    if (lambda->isInitCapture(&capture) || (capture.getCaptureKind() != clang::LCK_ByRef &&
                                   capture.getCaptureKind() != clang::LCK_This)) return false;
  }
  const clang::Stmt *node = lambda;
  for (unsigned depth = 0; depth != 8; ++depth) {
    const auto *p = parent(node, context);
    if (const auto *call = clang::dyn_cast_or_null<clang::CXXOperatorCallExpr>(p)) {
      if (call->getOperator() != clang::OO_Call || call->getNumArgs() == 0 || call->getArg(0) != node ||
          call->getDirectCallee() != lambda->getCallOperator()) return false;
      ImmediateLambdaControl control;
      return control.TraverseStmt(lambda->getBody()) && control.Safe;
    }
    if (const auto *cast = clang::dyn_cast_or_null<clang::ImplicitCastExpr>(p)) {
      if (cast->getCastKind() != clang::CK_NoOp) return false;
    } else if (!p || !clang::isa<clang::ParenExpr, clang::MaterializeTemporaryExpr, clang::ExprWithCleanups>(p)) return false;
    node = p;
  }
  return false;
}
class Uses : public clang::RecursiveASTVisitor<Uses> {
 public:
  clang::ASTContext &C; const clang::VarDecl *V; bool Safe = true; unsigned Count = 0;
  Uses(clang::ASTContext &c, const clang::VarDecl *v) : C(c), V(v) {}
  bool VisitStmt(clang::Stmt *) { if (++Count > 20000) { Safe = false; return false; } return true; }
  bool VisitAsmStmt(clang::AsmStmt *) { Safe = false; return true; }
  bool VisitLambdaExpr(clang::LambdaExpr *e) {
    for (const auto &capture : e->captures())
      if (capture.capturesVariable() && storage(capture.getCapturedVar()) == V && !immediateLambda(e, C)) Safe = false;
    return true;
  }
  bool VisitDeclRefExpr(clang::DeclRefExpr *e) {
    if (storage(e->getDecl()) == V && !safeUse(e)) Safe = false;
    return true;
  }
  bool safeUse(const clang::Expr *reference) const {
    const clang::Stmt *node = reference;
    bool address = false;
    for (unsigned depth = 0; depth != 24; ++depth) {
      const auto *p = parent(node, C);
      if (!p) return false;
      if (const auto *lambda = clang::dyn_cast<clang::LambdaExpr>(p)) {
        // Exempt only the compiler's exact reference-capture initializer. Body
        // references must independently reach a supported nonescaping use.
        auto init = lambda->capture_init_begin();
        for (const auto &capture : lambda->captures()) {
          const auto *initializer = *init++;
          if (initializer == node && node == reference && capture.capturesVariable() &&
              capture.getCaptureKind() == clang::LCK_ByRef && !lambda->isInitCapture(&capture) &&
              storage(capture.getCapturedVar()) == V) return immediateLambda(lambda, C);
        }
        return false;
      }
      if (auto *cast = clang::dyn_cast<clang::ImplicitCastExpr>(p)) {
        if (cast->getCastKind() == clang::CK_LValueToRValue && !address) return true;
        if (cast->getCastKind() == clang::CK_ArrayToPointerDecay) address = true;
        else if (cast->getCastKind() != clang::CK_NoOp && cast->getCastKind() != clang::CK_BitCast) return false;
        node = p; continue;
      }
      if (clang::isa<clang::ParenExpr>(p)) { node = p; continue; }
      if (auto *member = clang::dyn_cast<clang::MemberExpr>(p)) {
        if (!address && !member->isArrow() && standardPath(V->getType(), C)) {
          const auto *call = clang::dyn_cast_or_null<clang::CXXMemberCallExpr>(parent(member, C));
          return call && call->getCallee()->IgnoreParenImpCasts() == member && pathQuery(call, C);
        }
        if (!address && !member->isArrow() && standardVector(V->getType(), C)) {
          const auto *method = clang::dyn_cast<clang::CXXMethodDecl>(member->getMemberDecl());
          const auto *call = clang::dyn_cast_or_null<clang::CXXMemberCallExpr>(parent(member, C));
          if (method && method->getIdentifier() && call && C.getSourceManager().isInSystemHeader(method->getLocation()) &&
              standardVector(C.getRecordType(method->getParent()), C) && call->getCallee()->IgnoreParenImpCasts() == member) {
            const auto name = method->getName();
            // These calls cannot export the owner or an element reference. An
            // allocation callback cannot reach an otherwise unescaped owner.
            if ((name == "size" || name == "empty") && call->getNumArgs() == 0) return true;
            if (name == "push_back" && call->getNumArgs() == 1 && call->getArg(0)->getType()->isArithmeticType()) return true;
          }
          return false;
        }
        const auto *field = clang::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
        if (address || member->isArrow() || !field || field->getType()->isReferenceType() || field->getParent()->isUnion()) return false;
        node = p; continue;
      }
      if (auto *subscript = clang::dyn_cast<clang::ArraySubscriptExpr>(p)) {
        // This visitor establishes nonescape, not bounds. A direct array
        // element lvalue does not export the array; callers must separately
        // prove the index when proving that element access safe.
        const auto *base = clang::dyn_cast<clang::DeclRefExpr>(subscript->getBase()->IgnoreParenImpCasts());
        if (owner(subscript, C) != V &&
            !(base && storage(base->getDecl()) == V && V->getType()->isArrayType())) return false;
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
        const bool characterArray = V->getType()->isArrayType() && C.getBaseElementType(V->getType())->isCharType();
        // These actual C library functions use their character buffers only
        // during the call and return no alias to the storage.
        if (characterArray && name == "strcmp" && call->getNumArgs() == 2 &&
            (call->getArg(0) == node || call->getArg(1) == node)) return true;
        const unsigned buffer = name == "read" || (characterArray && name == "readlink") ? 1 : name == "memset" ? 0 : 99;
        if (buffer >= call->getNumArgs() || call->getArg(buffer) != node) return false;
        // memset returns its destination; retaining that result creates an alias.
        return name != "memset" || discarded(call, C);
      }
      if (auto *construct = clang::dyn_cast<clang::CXXConstructExpr>(p)) {
        auto *constructor = construct->getConstructor();
        return !address && constructor && constructor->isCopyOrMoveConstructor() &&
            (constructor->isTrivial() ||
             (standardVector(V->getType(), C) && standardVector(construct->getType(), C) &&
              C.hasSameUnqualifiedType(V->getType(), construct->getType()) &&
              C.getSourceManager().isInSystemHeader(constructor->getLocation())));
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
  bool live(const clang::Expr *expression, unsigned recursionDepth = 0) const {
    if (!expression || recursionDepth > 8 || expression->isInstantiationDependent() || expression->getType().isNull() || expression->getType()->isUnionType()) return false;
    const auto *v = owner(expression, C);
    if (!v || v->hasAttr<clang::CleanupAttr>() || !v->hasLocalStorage() || clang::isa<clang::ParmVarDecl>(v) || !shape(v->getType(), C)) return false;
    if (v->hasInit()) {
      const auto *initial = v->getInit()->IgnoreParenImpCasts();
      if (standardPath(v->getType(), C)) {
        for (unsigned wraps = 0; wraps != 8; ++wraps) {
          if (const auto *cleanup = clang::dyn_cast<clang::ExprWithCleanups>(initial))
            initial = cleanup->getSubExpr()->IgnoreParenImpCasts();
          else if (const auto *temporary = clang::dyn_cast<clang::CXXBindTemporaryExpr>(initial)) {
            if (!standardPath(temporary->getType(), C) ||
                !C.getSourceManager().isInSystemHeader(temporary->getTemporary()->getDestructor()->getLocation())) return false;
            initial = temporary->getSubExpr()->IgnoreParenImpCasts();
          } else break;
        }
      }
      if (initial->isInstantiationDependent() || initial->getType().isNull()) return false;
      if (standardPath(v->getType(), C)) {
        // Admit only an exact native std::string lvalue source. Arbitrary
        // factories can publish the eventual result's address through NRVO.
        if (const auto *construct = clang::dyn_cast<clang::CXXConstructExpr>(initial)) {
          const auto *constructor = construct->getConstructor();
          if (!constructor || !C.getSourceManager().isInSystemHeader(constructor->getLocation()) ||
              constructor->isCopyOrMoveConstructor() || construct->getNumArgs() < 1 || construct->getNumArgs() > 2 ||
              !nativeString(construct->getArg(0)->getType(), C) ||
              !clang::isa<clang::DeclRefExpr>(construct->getArg(0)->IgnoreParenImpCasts())) return false;
          if (construct->getNumArgs() == 2) {
            const clang::Expr *format = construct->getArg(1);
            if (const auto *argument = clang::dyn_cast<clang::CXXDefaultArgExpr>(format)) format = argument->getExpr();
            if (!format->isCXX11ConstantExpr(C)) return false;
          }
        } else {
          const auto *call = clang::dyn_cast<clang::CXXMemberCallExpr>(initial);
          if (!pathQuery(call, C) || call->getMethodDecl()->getName() != "parent_path" ||
              !clang::isa<clang::DeclRefExpr>(call->getImplicitObjectArgument()->IgnoreParenImpCasts()) ||
              !live(call->getImplicitObjectArgument(), recursionDepth + 1)) return false;
        }
      } else if (const auto *construct = clang::dyn_cast<clang::CXXConstructExpr>(initial)) {
        const auto *constructor = construct->getConstructor();
        if (!constructor) return false;
        if (standardVector(v->getType(), C)) {
          if (!constructor->isDefaultConstructor() || construct->getNumArgs() != 0 ||
              !C.getSourceManager().isInSystemHeader(constructor->getLocation())) return false;
        } else if (!constructor->isTrivial()) {
          if (!constructor->isDefaulted() || !constructor->isDefaultConstructor()) return false;
          for (const auto *field : constructor->getParent()->fields())
            if (!constantFieldInitializer(field, C)) return false;
        }
      } else if (const auto *list = clang::dyn_cast<clang::InitListExpr>(initial)) {
        AggregateInitialization provenance(C);
        if (!provenance.TraverseStmt(const_cast<clang::InitListExpr *>(list)) || !provenance.Safe) return false;
      } else if (!initial->isCXX11ConstantExpr(C) && !parserResult(initial, C)) return false;
    }
    const auto declarationParents = C.getParents(*v);
    if (declarationParents.size() != 1) return false;
    const auto *declaration = declarationParents[0].get<clang::DeclStmt>();
    const auto *scope = declaration ? clang::dyn_cast_or_null<clang::CompoundStmt>(parent(declaration, C)) : nullptr;
    if (!scope) return false;
    const clang::Stmt *useStatement = expression;
    for (unsigned depth = 0; useStatement && depth != 64 && useParent(useStatement, C) != scope; ++depth) useStatement = useParent(useStatement, C);
    if (!useStatement || useParent(useStatement, C) != scope || useStatement == declaration) return false;
    // A later goto may revisit a use after a later escape. Reject such functions.
    const auto *function = clang::dyn_cast<clang::FunctionDecl>(v->getDeclContext());
    if (!function || !function->getBody()) return false;
    ControlFlow flow;
    if (!flow.TraverseStmt(const_cast<clang::Stmt *>(function->getBody())) || !flow.Safe) return false;
    // An alias exported by an earlier iteration may reach replacement storage
    // at the same address. Inspect every enclosing loop, including later uses.
    bool enclosedByLoop = false;
    for (const clang::Stmt *ancestor = scope; ancestor; ancestor = parent(ancestor, C)) {
      if (clang::isa<clang::ForStmt, clang::CXXForRangeStmt, clang::WhileStmt, clang::DoStmt>(ancestor)) {
        enclosedByLoop = true;
        Uses loopUses(C, v);
        if (!loopUses.TraverseStmt(const_cast<clang::Stmt *>(ancestor)) || !loopUses.Safe) return false;
      }
    }
    Uses uses(C, v);
    bool started = false;
    for (const auto *statement : scope->body()) {
      if (statement == declaration) started = true;
      if (!started) continue;
      // A path query in an if condition executes before either branch. Later
      // branches may legitimately export the owner; they cannot affect this
      // first query. Retain whole-loop scans for repeated storage epochs.
      if (statement == useStatement && !enclosedByLoop && standardPath(v->getType(), C)) {
        const auto *test = clang::dyn_cast<clang::IfStmt>(statement);
        if (test && !test->getInit() && !test->getConditionVariable()) {
          bool inCondition = false;
          for (const clang::Stmt *node = expression; node && node != statement; node = parent(node, C))
            if (node == test->getCond()) { inCondition = true; break; }
          if (inCondition)
            return uses.TraverseStmt(const_cast<clang::Expr *>(test->getCond())) && uses.Safe;
        }
      }
      if (!uses.TraverseStmt(const_cast<clang::Stmt *>(statement)) || !uses.Safe) return false;
      if (statement == useStatement) return true;
    }
    return false;
  }
 private:
  clang::ASTContext &C;
};
} // namespace baseline_lifetime
