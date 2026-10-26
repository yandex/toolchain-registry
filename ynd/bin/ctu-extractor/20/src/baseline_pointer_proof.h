#pragma once

#include "clang/AST/ASTContext.h"
#include "clang/Basic/SourceManager.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "llvm/ADT/DenseSet.h"

// Same-array proofs deliberately require a bounded position, not just a shared
// allocation name: p + 100 must not become safe merely because p is in an array.
namespace baseline_pointer {
inline const clang::VarDecl *variable(const clang::Expr *e) {
  if (!e) return nullptr;
  if (auto *r = clang::dyn_cast<clang::DeclRefExpr>(e->IgnoreParenImpCasts()))
    return clang::dyn_cast<clang::VarDecl>(r->getDecl());
  return nullptr;
}
inline bool standard(const clang::DeclContext *c) {
  for (; c; c = c->getParent())
    if (auto *n = clang::dyn_cast<clang::NamespaceDecl>(c))
      if (n->isStdNamespace()) return true;
  return false;
}
inline bool exits(const clang::Stmt *s) {
  if (auto *c = clang::dyn_cast_or_null<clang::CompoundStmt>(s))
    return !c->body_empty() && exits(c->body_back());
  return s && (clang::isa<clang::ReturnStmt>(s) || clang::isa<clang::CXXThrowExpr>(s));
}
struct Position {
  const clang::VarDecl *Owner = nullptr;
  bool Interior = false;
  explicit operator bool() const { return Owner != nullptr; }
};
class Proof {
 public:
  explicit Proof(clang::ASTContext &context) : C(context) {}
  bool boundedPosition(const clang::Expr *e) {
    // These two standard const queries occur in one expression with no
    // intervening user code. A referenced view may change elsewhere without
    // invalidating the relation between its current data and current size.
    const auto *add = clang::dyn_cast_or_null<clang::BinaryOperator>(e);
    if (add && add->getOpcode() == clang::BO_Add) {
      const auto *data = clang::dyn_cast<clang::CXXMemberCallExpr>(add->getLHS()->IgnoreParenImpCasts());
      const auto *size = clang::dyn_cast<clang::CXXMemberCallExpr>(add->getRHS()->IgnoreParenImpCasts());
      if (data && size && data->getMethodDecl() && size->getMethodDecl() &&
          data->getMethodDecl()->getNameAsString() == "data" && size->getMethodDecl()->getNameAsString() == "size" &&
          data->getMethodDecl()->isConst() && size->getMethodDecl()->isConst() &&
          data->getMethodDecl()->getParent() == size->getMethodDecl()->getParent() &&
          data->getMethodDecl()->getParent()->getName() == "basic_string_view" && standard(data->getMethodDecl()->getDeclContext())) {
        const auto *owner = variable(data->getImplicitObjectArgument());
        if (owner && owner == variable(size->getImplicitObjectArgument())) return true;
      }
    }
    return bool(position(e, e, 0));
  }
  bool boundedSubscript(const clang::ArraySubscriptExpr *e) {
    const auto *base = e->getBase()->IgnoreParenImpCasts();
    const auto *array = C.getAsConstantArrayType(base->getType());
    if (!array) return false;
    uint64_t maximum = 0;
    return unsignedMaximum(e->getIdx(), maximum, 0) && maximum < array->getSize().getLimitedValue();
  }
  bool safeIntegerIncrement(const clang::UnaryOperator *e) {
    if (!e->isIncrementOp() || !e->getType()->isSignedIntegerType()) return false;
    const auto *v = variable(e->getSubExpr());
    if (!v || !v->hasLocalStorage() || !v->hasInit()) return false;
    auto node = clang::DynTypedNode::create(*e);
    for (unsigned depth = 0; depth < 12; ++depth) {
      auto parents = C.getParents(node); if (parents.size() != 1) return false;
      node = parents[0];
      const auto *loop = node.get<clang::ForStmt>();
      if (!loop) continue;
      if (!loop->getCond() || !loop->getInc() || loop->getInc()->IgnoreParenImpCasts() != e) return false;
      const auto *decls = clang::dyn_cast_or_null<clang::DeclStmt>(loop->getInit());
      if (!decls || !decls->isSingleDecl() || decls->getSingleDecl() != v) return false;
      const auto *condition = clang::dyn_cast_or_null<clang::BinaryOperator>(loop->getCond()->IgnoreParenImpCasts());
      if (!condition || variable(condition->getLHS()) != v || condition->getOpcode() != clang::BO_LT) return false;
      auto start = v->getInit()->getIntegerConstantExpr(C), end = condition->getRHS()->getIntegerConstantExpr(C);
      const unsigned bits = C.getIntWidth(v->getType());
      if (!start || !end || start->isNegative() || end->isNegative() || (bits < 2 || bits > 64) ||
          end->getLimitedValue() > (UINT64_MAX >> (65 - bits)) || start->getLimitedValue() > end->getLimitedValue()) return false;
      Writes writes(v);
      writes.ResetContext = &C;
      writes.ResetLimit = end->getLimitedValue();
      writes.TraverseStmt(const_cast<clang::Stmt *>(loop->getBody()));
      return !writes.Unknown && writes.Increments.empty();
    }
    return false;
  }
  bool safeRangeIncrement(const clang::UnaryOperator *e) {
    if (!e->isIncrementOp() || !e->getSubExpr()->getType()->isPointerType()) return false;
    auto node = clang::DynTypedNode::create(*e);
    for (unsigned depth = 0; depth < 12; ++depth) {
      auto parents = C.getParents(node); if (parents.size() != 1) return false;
      node = parents[0];
      const auto *loop = node.get<clang::CXXForRangeStmt>();
      if (!loop) continue;
      if (!loop->getInc() || loop->getInc()->IgnoreParenImpCasts() != e) return false;
      const auto *init = initializer(loop->getRangeInit());
      if (!init) return false;
      // Braced range-for initializer_list storage has loop scope and compiler
      // generated iterators; users cannot rebind the hidden range object.
      if (clang::isa<clang::CXXStdInitializerListExpr>(init)) {
        const auto *iterator = variable(e->getSubExpr());
        if (!iterator) return false;
        Writes writes(iterator); writes.TraverseStmt(const_cast<clang::Stmt *>(loop->getBody()));
        return !writes.Unknown && writes.Increments.empty();
      }
      const auto *owner = variable(init);
      if (!owner || !owner->hasLocalStorage() || owner->getType()->isReferenceType() || !owner->getType().isConstQualified()) return false;
      const auto *record = owner->getType()->getAsCXXRecordDecl();
      if (!record || !standard(record->getDeclContext()) || (record->getName() != "vector" && record->getName() != "basic_string")) return false;
      const auto *f = function(owner);
      if (!f || !f->hasBody()) return false;
      Writes writes(owner);
      if (const auto *range = loop->getRangeStmt())
        if (range->isSingleDecl()) writes.IgnoredBinding = clang::dyn_cast<clang::VarDecl>(range->getSingleDecl());
      writes.TraverseStmt(const_cast<clang::Stmt *>(f->getBody()));
      return !writes.Unknown && writes.Increments.empty();
    }
    return false;
  }
  bool sameArray(const clang::Expr *a, const clang::Expr *b, const clang::Expr *use) {
    auto left = position(a, use, 0), right = position(b, use, 0);
    return left && right && left.Owner == right.Owner;
  }
 private:
  clang::ASTContext &C;
  llvm::DenseSet<const clang::VarDecl *> Active;
  bool unsignedMaximum(const clang::Expr *e, uint64_t &maximum, unsigned depth) {
    if (!e || depth > 12 || e->isValueDependent()) return false;
    if (auto value = e->getIntegerConstantExpr(C)) {
      if (value->isNegative() || value->getActiveBits() > 63) return false;
      maximum = value->getZExtValue(); return true;
    }
    e = e->IgnoreParenImpCasts();
    if (e->getType()->isUnsignedIntegerType()) {
      const unsigned bits = C.getIntWidth(e->getType());
      if (bits < 64) { maximum = (uint64_t(1) << bits) - 1; }
      else maximum = UINT64_MAX;
      // Refine masks and shifts below before falling back to the type range.
    } else maximum = UINT64_MAX;
    if (const auto *op = clang::dyn_cast<clang::BinaryOperator>(e)) {
      if (op->getOpcode() == clang::BO_And) {
        for (const auto *operand : {op->getLHS(), op->getRHS()})
          if (auto mask = operand->getIntegerConstantExpr(C))
            if (!mask->isNegative() && mask->getActiveBits() < 64) {
              maximum = mask->getZExtValue(); return true;
            }
      }
      if (op->getOpcode() == clang::BO_Shr) {
        auto shift = op->getRHS()->getIntegerConstantExpr(C);
        uint64_t input;
        if (shift && !shift->isNegative() && shift->getLimitedValue() < C.getIntWidth(op->getLHS()->getType()) &&
            unsignedMaximum(op->getLHS(), input, depth + 1)) {
          maximum = input >> shift->getLimitedValue(); return true;
        }
      }
    }
    if (e->getType()->isUnsignedIntegerType()) return true;
    if (const auto *v = variable(e)) {
      const auto *call = v->hasInit() ? clang::dyn_cast<clang::CallExpr>(v->getInit()->IgnoreParenImpCasts()) : nullptr;
      const auto *callee = call ? call->getDirectCallee() : nullptr;
      if (callee && callee->getIdentifier() && callee->getName() == "readlink" && callee->isExternC() &&
          C.getSourceManager().isInSystemHeader(callee->getLocation()) && call->getNumArgs() == 3 && nonnegative(v, e)) {
        auto length = call->getArg(2)->getIntegerConstantExpr(C);
        const auto *f = function(v);
        if (length && !length->isNegative() && length->getActiveBits() < 63 && f && f->hasBody()) {
          Writes writes(v); writes.TraverseStmt(const_cast<clang::Stmt *>(f->getBody()));
          if (!writes.Unknown && writes.Increments.empty()) { maximum = length->getZExtValue(); return true; }
        }
      }
    }
    return false;
  }
  bool nonnegative(const clang::VarDecl *v, const clang::Expr *use) {
    auto node = clang::DynTypedNode::create(*use);
    for (unsigned depth = 0; depth < 64; ++depth) {
      auto parents = C.getParents(node);
      if (parents.size() != 1) break;
      const auto *child = node.get<clang::Stmt>(); node = parents[0];
      if (const auto *block = node.get<clang::CompoundStmt>()) {
        for (const auto *statement : block->body()) {
          if (statement == child) break;
          const auto *test = clang::dyn_cast<clang::IfStmt>(statement);
          if (!test) continue;
          const clang::Stmt *then = test->getThen();
          if (const auto *body = clang::dyn_cast<clang::CompoundStmt>(then))
            then = body->body_empty() ? nullptr : body->body_back();
          if (!exits(then) && !clang::isa_and_nonnull<clang::ContinueStmt>(then)) continue;
          const auto *condition = clang::dyn_cast<clang::BinaryOperator>(test->getCond()->IgnoreParenImpCasts());
          if (!condition || variable(condition->getLHS()) != v) continue;
          if (condition->getOpcode() != clang::BO_LT && condition->getOpcode() != clang::BO_LE) continue;
          auto zero = condition->getRHS()->getIntegerConstantExpr(C);
          if (zero && zero->isZero()) return true;
        }
      }
    }
    return false;
  }
  bool positivePointer(const clang::Expr *e, const clang::VarDecl *v) {
    e = e->IgnoreParenImpCasts();
    if (variable(e) == v) return true;
    const auto *test = clang::dyn_cast<clang::BinaryOperator>(e);
    if (!test || test->getOpcode() != clang::BO_NE) return false;
    return (variable(test->getLHS()) == v && test->getRHS()->isNullPointerConstant(C, clang::Expr::NPC_NeverValueDependent)) ||
           (variable(test->getRHS()) == v && test->getLHS()->isNullPointerConstant(C, clang::Expr::NPC_NeverValueDependent));
  }
  bool nonnull(const clang::VarDecl *v, const clang::Expr *use) {
    clang::DynTypedNode node = clang::DynTypedNode::create(*use);
    for (unsigned depth = 0; depth != 64; ++depth) {
      auto parents = C.getParents(node);
      if (parents.size() != 1) break;
      const auto *child = node.get<clang::Stmt>();
      node = parents[0];
      if (const auto *logical = node.get<clang::BinaryOperator>())
        if (logical->getOpcode() == clang::BO_LAnd && logical->getRHS() == child && positivePointer(logical->getLHS(), v)) return true;
      if (const auto *branch = node.get<clang::IfStmt>())
        if (branch->getThen() == child && positivePointer(branch->getCond(), v)) return true;
      if (const auto *block = node.get<clang::CompoundStmt>()) {
        for (const auto *statement : block->body()) {
          if (statement == child) break;
          const auto *test = clang::dyn_cast<clang::IfStmt>(statement);
          if (!test || !exits(test->getThen())) continue;
          const auto *condition = test->getCond()->IgnoreParenImpCasts();
          if (const auto *negation = clang::dyn_cast<clang::UnaryOperator>(condition))
            if (negation->getOpcode() == clang::UO_LNot && variable(negation->getSubExpr()) == v) return true;
          const auto *equal = clang::dyn_cast<clang::BinaryOperator>(condition);
          if (!equal || equal->getOpcode() != clang::BO_EQ) continue;
          if ((variable(equal->getLHS()) == v && equal->getRHS()->isNullPointerConstant(C, clang::Expr::NPC_NeverValueDependent)) ||
              (variable(equal->getRHS()) == v && equal->getLHS()->isNullPointerConstant(C, clang::Expr::NPC_NeverValueDependent))) return true;
        }
      }
    }
    return false;
  }
  const clang::FunctionDecl *function(const clang::VarDecl *v) {
    for (auto *dc = v->getDeclContext(); dc; dc = dc->getParent())
      if (const auto *f = clang::dyn_cast<clang::FunctionDecl>(dc)) return f;
    return nullptr;
  }
  class Writes : public clang::RecursiveASTVisitor<Writes> {
   public:
    bool shouldVisitImplicitCode() const { return true; }
    const clang::VarDecl *V;
    bool Unknown = false;
    clang::ASTContext *ResetContext = nullptr;
    uint64_t ResetLimit = 0;
    const clang::VarDecl *IgnoredBinding = nullptr;
    llvm::SmallVector<const clang::UnaryOperator *, 4> Increments;
    explicit Writes(const clang::VarDecl *v) : V(v) {}
    bool aliases(const clang::Expr *e) {
      if (!e) return false;
      e = e->IgnoreParenImpCasts();
      if (const auto *cast = clang::dyn_cast<clang::ExplicitCastExpr>(e))
        if (cast->isGLValue()) return aliases(cast->getSubExpr());
      if (const auto *conditional = clang::dyn_cast<clang::ConditionalOperator>(e))
        return aliases(conditional->getTrueExpr()) || aliases(conditional->getFalseExpr());
      if (const auto *comma = clang::dyn_cast<clang::BinaryOperator>(e))
        if (comma->getOpcode() == clang::BO_Comma) return aliases(comma->getRHS());
      return variable(e) == V;
    }
    bool VisitInitListExpr(clang::InitListExpr *e) {
      for (const auto *init : e->inits()) if (init->isGLValue() && aliases(init)) Unknown = true;
      return true;
    }
    bool VisitExplicitCastExpr(clang::ExplicitCastExpr *e) {
      if (e->isGLValue() && aliases(e)) Unknown = true;
      return true;
    }
    bool VisitBinaryOperator(clang::BinaryOperator *e) {
      if (e->isAssignmentOp() && aliases(e->getLHS())) {
        if (ResetContext && e->getOpcode() == clang::BO_Assign) {
          auto value = e->getRHS()->getIntegerConstantExpr(*ResetContext);
          if (value && !value->isNegative() && value->getLimitedValue() < ResetLimit) return true;
        }
        Unknown = true;
      }
      return true;
    }
    bool VisitUnaryOperator(clang::UnaryOperator *e) {
      if (!aliases(e->getSubExpr())) return true;
      if (variable(e->getSubExpr()) != V) { Unknown = true; return true; }
      if (e->getOpcode() == clang::UO_AddrOf) Unknown = true;
      if (e->isIncrementDecrementOp()) {
        if (e->isIncrementOp()) Increments.push_back(e); else Unknown = true;
      }
      return true;
    }
    bool VisitVarDecl(clang::VarDecl *d) {
      if (d != IgnoredBinding && d->getType()->isReferenceType() && aliases(d->getInit())) Unknown = true;
      return true;
    }
    bool VisitCallExpr(clang::CallExpr *e) {
      for (auto *a : e->arguments()) {
        if (a->isGLValue() && aliases(a)) Unknown = true;
        // Calling a local lambda is not an escape of its closure object.
        if (captures(a)) {
          auto *op = clang::dyn_cast<clang::CXXOperatorCallExpr>(e);
          if (!op || op->getOperator() != clang::OO_Call || a != op->getArg(0)) Unknown = true;
        }
      }
      return true;
    }
    bool VisitAsmStmt(clang::AsmStmt *) { Unknown = true; return true; }
    bool VisitGotoStmt(clang::GotoStmt *) { Unknown = true; return true; }
    bool VisitIndirectGotoStmt(clang::IndirectGotoStmt *) { Unknown = true; return true; }
    bool VisitLabelStmt(clang::LabelStmt *) { Unknown = true; return true; }
    bool captures(const clang::Expr *e) {
      e = e->IgnoreParenImpCasts();
      if (auto *v = variable(e)) {
        if (!v->hasInit()) return false;
        e = v->getInit()->IgnoreParenImpCasts();
      }
      if (auto *clean = clang::dyn_cast<clang::ExprWithCleanups>(e)) e = clean->getSubExpr();
      if (auto *lambda = clang::dyn_cast<clang::LambdaExpr>(e))
        for (const auto &capture : lambda->captures())
          if (capture.capturesVariable() && capture.getCapturedVar() == V && capture.getCaptureKind() == clang::LCK_ByRef) return true;
      return false;
    }
    bool VisitReturnStmt(clang::ReturnStmt *s) {
      if (s->getRetValue() && (captures(s->getRetValue()) || (s->getRetValue()->isGLValue() && aliases(s->getRetValue())))) Unknown = true;
      return true;
    }
    bool VisitCXXConstructExpr(clang::CXXConstructExpr *e) {
      for (auto *a : e->arguments()) if (captures(a) || (a->isGLValue() && aliases(a))) Unknown = true;
      return true;
    }
    bool VisitCXXMemberCallExpr(clang::CXXMemberCallExpr *e) {
      if (aliases(e->getImplicitObjectArgument())) {
        const auto *m = e->getMethodDecl();
        if (!m || !standard(m->getDeclContext()) || !m->isConst()) Unknown = true;
      }
      return true;
    }
  };
  bool boundedIncrement(const clang::UnaryOperator *inc, const clang::VarDecl *v,
                        Position initial, const clang::Expr *use, unsigned depth) {
    clang::DynTypedNode node = clang::DynTypedNode::create(*inc);
    for (unsigned i = 0; i != 32; ++i) {
      auto parents = C.getParents(node);
      if (parents.size() != 1) return false;
      node = parents[0];
      if (node.get<clang::ForStmt>() || node.get<clang::DoStmt>() ||
          node.get<clang::CXXForRangeStmt>() || node.get<clang::LambdaExpr>()) return false;
      if (const auto *loop = node.get<clang::WhileStmt>()) {
        const clang::Expr *condition = loop->getCond()->IgnoreParenImpCasts();
        while (const auto *logical = clang::dyn_cast<clang::BinaryOperator>(condition)) {
          if (logical->getOpcode() != clang::BO_LAnd) break;
          condition = logical->getLHS()->IgnoreParenImpCasts();
        }
        const auto *less = clang::dyn_cast<clang::BinaryOperator>(condition);
        if (!less || less->getOpcode() != clang::BO_LT || variable(less->getLHS()) != v) return false;
        auto end = position(less->getRHS(), use, depth + 1);
        return end && end.Owner == initial.Owner;
      }
      if (node.get<clang::FunctionDecl>()) return false;
    }
    return false;
  }
  class ReferenceEffects : public clang::RecursiveASTVisitor<ReferenceEffects> {
   public:
    clang::ASTContext &C;
    explicit ReferenceEffects(clang::ASTContext &context) : C(context) {}
    bool Unknown = false;
    bool VisitCallExpr(clang::CallExpr *e) {
      const auto *f = e->getDirectCallee();
      if (!f || !standard(f->getDeclContext())) { Unknown = true; return true; }
      const auto name = f->getNameAsString();
      if (name == "from_chars" || name == "move" || name == "forward") return true;
      if (const auto *method = clang::dyn_cast<clang::CXXMethodDecl>(f))
        if (method->isConst() && method->getParent()->getName() == "basic_string_view" &&
            (name == "data" || name == "size" || name == "empty" || name == "begin" || name == "end")) return true;
      Unknown = true; return true;
    }
    bool VisitCXXConstructExpr(clang::CXXConstructExpr *e) {
      const auto *f = e->getConstructor();
      if (f && f->isTrivial()) return true;
      // The library pair constructor only copies scalar values in this narrow
      // case. Record elements or record arguments can run user constructors or
      // conversions, even when the outer constructor belongs to std::pair.
      bool scalarPair = f && standard(f->getDeclContext()) && f->getParent()->getName() == "pair" &&
          C.getSourceManager().isInSystemHeader(f->getLocation());
      unsigned fields = 0;
      if (scalarPair) for (const auto *field : f->getParent()->fields()) {
        ++fields;
        scalarPair &= field->getType()->isScalarType();
      }
      scalarPair &= fields == 2;
      for (const auto *argument : e->arguments())
        scalarPair &= argument->getType()->isScalarType();
      if (!scalarPair) Unknown = true;
      return true;
    }
    bool VisitAsmStmt(clang::AsmStmt *) { Unknown = true; return true; }
  };
  const clang::Expr *initializer(const clang::Expr *e) {
    if (!e) return nullptr;
    e = e->IgnoreParenImpCasts();
    if (const auto *clean = clang::dyn_cast<clang::ExprWithCleanups>(e)) return initializer(clean->getSubExpr());
    if (const auto *temp = clang::dyn_cast<clang::MaterializeTemporaryExpr>(e)) return initializer(temp->getSubExpr());
    return e;
  }
  bool terminatedArray(const clang::VarDecl *v) {
    const auto *array = v ? C.getAsConstantArrayType(v->getType()) : nullptr;
    if (!array || !array->getElementType()->isCharType() || !v->hasLocalStorage() || !v->hasInit()) return false;
    const uint64_t size = array->getSize().getLimitedValue();
    if (!size) return false;
    const auto *init = v->getInit()->IgnoreParenImpCasts();
    bool terminated = false;
    if (const auto *literal = clang::dyn_cast<clang::StringLiteral>(init)) terminated = literal->getLength() < size;
    if (const auto *list = clang::dyn_cast<clang::InitListExpr>(init)) {
      if (list->getNumInits() < size) terminated = true;
      else if (auto last = list->getInit(size - 1)->getIntegerConstantExpr(C)) terminated = last->isZero();
    }
    if (!terminated) return false;
    class Uses : public clang::RecursiveASTVisitor<Uses> {
     public:
      clang::ASTContext &C; const clang::VarDecl *V; uint64_t Size; bool Unknown = false;
      Uses(clang::ASTContext &c, const clang::VarDecl *v, uint64_t size) : C(c), V(v), Size(size) {}
      llvm::DenseSet<const clang::VarDecl *> SearchAliases;
      bool readOnlyAlias(clang::DeclRefExpr *ref) {
        auto node = clang::DynTypedNode::create(*ref);
        for (unsigned depth = 0; depth < 16; ++depth) {
          auto parents = C.getParents(node); if (parents.size() != 1) return false;
          node = parents[0];
          if (node.get<clang::ImplicitCastExpr>() || node.get<clang::ParenExpr>()) continue;
          if (const auto *binary = node.get<clang::BinaryOperator>()) {
            if (binary->isComparisonOp() || binary->isLogicalOp()) return true;
            if (binary->getOpcode() == clang::BO_Add) continue;
            return false;
          }
          if (const auto *unary = node.get<clang::UnaryOperator>()) return unary->getOpcode() == clang::UO_LNot;
          if (const auto *call = node.get<clang::CXXOperatorCallExpr>()) {
            const auto *callee = call->getDirectCallee();
            if (!callee || !standard(callee->getDeclContext()) || call->getOperator() != clang::OO_EqualEqual) return false;
            for (const auto *arg : call->arguments()) {
              const auto *record = clang::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(arg->getType()->getAsCXXRecordDecl());
              if (!record) continue;
              if (!standard(record->getDeclContext()) || record->getName() != "basic_string" || record->getTemplateArgs().size() < 2) return false;
              const auto &traitsArg = record->getTemplateArgs()[1];
              const auto *traits = traitsArg.getKind() == clang::TemplateArgument::Type ? traitsArg.getAsType()->getAsCXXRecordDecl() : nullptr;
              return traits && standard(traits->getDeclContext()) && traits->getName() == "char_traits";
            }
            return false;
          }
          if (node.get<clang::IfStmt>() || node.get<clang::CompoundStmt>()) return true;
          return false;
        }
        return false;
      }
      bool searchResultReadOnly(const clang::CallExpr *call) {
        auto node = clang::DynTypedNode::create(*call);
        for (unsigned depth = 0; depth < 8; ++depth) {
          auto parents = C.getParents(node); if (parents.size() != 1) return false;
          node = parents[0];
          if (node.get<clang::ImplicitCastExpr>() || node.get<clang::ParenExpr>()) continue;
          if (const auto *alias = node.get<clang::VarDecl>()) {
            if (!alias->hasLocalStorage() || !alias->getType()->isPointerType() ||
                !alias->getType()->getPointeeType().isConstQualified()) return false;
            SearchAliases.insert(alias); return true;
          }
          return node.get<clang::CompoundStmt>() != nullptr;
        }
        return false;
      }
      bool VisitDeclRefExpr(clang::DeclRefExpr *ref) {
        if (const auto *alias = clang::dyn_cast<clang::VarDecl>(ref->getDecl()))
          if (SearchAliases.contains(alias)) { if (!readOnlyAlias(ref)) Unknown = true; return true; }
        if (ref->getDecl() != V) return true;
        auto node = clang::DynTypedNode::create(*ref);
        for (unsigned depth = 0; depth < 6; ++depth) {
          auto parents = C.getParents(node); if (parents.size() != 1) break;
          node = parents[0];
          if (node.get<clang::ImplicitCastExpr>() || node.get<clang::ParenExpr>()) continue;
          if (node.get<clang::UnaryExprOrTypeTraitExpr>()) return true;
          const auto *call = node.get<clang::CallExpr>();
          const auto *f = call ? call->getDirectCallee() : nullptr;
          if (!f || !f->getIdentifier() || !C.getSourceManager().isInSystemHeader(f->getLocation()) ||
              !f->getDeclContext()->getRedeclContext()->isTranslationUnit()) break;
          const auto name = f->getName();
          if (name == "strchr" || name == "strrchr") {
            if (variable(call->getArg(0)) == V && searchResultReadOnly(call)) return true;
            break;
          }
          if (name == "strlen" || name == "strcmp" || name == "strncmp") return true;
          if ((name == "sscanf" || name == "__isoc99_sscanf") && call->getNumArgs() >= 3 && variable(call->getArg(2)) == V) {
            class Occurrences : public clang::RecursiveASTVisitor<Occurrences> {
             public:
              const clang::VarDecl *V; unsigned Count = 0;
              explicit Occurrences(const clang::VarDecl *v) : V(v) {}
              bool VisitDeclRefExpr(clang::DeclRefExpr *ref) { Count += ref->getDecl() == V; return true; }
            } occurrences(V);
            occurrences.TraverseStmt(const_cast<clang::CallExpr *>(call));
            if (occurrences.Count != 1) break;
            const auto *format = clang::dyn_cast<clang::StringLiteral>(call->getArg(1)->IgnoreParenImpCasts());
            if (!format) break;
            auto text = format->getString();
            // Recognize the first non-suppressed conversion only. Every other
            // use of this array remains unknown, including format arguments.
            for (size_t i = 0; i < text.size(); ++i) {
              if (text[i] != '%') continue;
              if (++i == text.size()) break;
              if (text[i] == '%') continue;
              const bool suppressed = text[i] == '*'; if (suppressed) ++i;
              uint64_t width = 0;
              while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
                if (width > Size) { Unknown = true; return true; }
                width = width * 10 + (text[i++] - '0');
              }
              if (i >= text.size()) break;
              if (suppressed) { if (text[i] != 's') break; continue; }
              if (text[i] == 's' && width > 0 && width < Size) return true;
              break;
            }
          }
          break;
        }
        Unknown = true; return true;
      }
      bool VisitAsmStmt(clang::AsmStmt *) { Unknown = true; return true; }
    } uses(C, v, size);
    const auto *f = function(v);
    if (!f || !f->hasBody()) return false;
    uses.TraverseStmt(const_cast<clang::Stmt *>(f->getBody()));
    return !uses.Unknown;
  }
  Position fromChars(const clang::Expr *e, const clang::Expr *use, unsigned depth) {
    const auto *call = clang::dyn_cast_or_null<clang::CallExpr>(initializer(e));
    const auto *f = call ? call->getDirectCallee() : nullptr;
    if (!f || !standard(f->getDeclContext()) || f->getNameAsString() != "from_chars" || call->getNumArgs() < 3) return {};
    auto first = position(call->getArg(0), use, depth + 1), last = position(call->getArg(1), use, depth + 1);
    if (!first || !last || first.Owner != last.Owner) return {};
    const auto *end = clang::dyn_cast<clang::BinaryOperator>(call->getArg(1)->IgnoreParenImpCasts());
    if (!end || end->getOpcode() != clang::BO_Add) return {};
    bool ordered = variable(call->getArg(0)) == first.Owner && variable(end->getLHS()) == first.Owner;
    const auto *beginCall = clang::dyn_cast<clang::CXXMemberCallExpr>(call->getArg(0)->IgnoreParenImpCasts());
    const auto *endBase = clang::dyn_cast<clang::CXXMemberCallExpr>(end->getLHS()->IgnoreParenImpCasts());
    if (beginCall && endBase && beginCall->getMethodDecl() && endBase->getMethodDecl() &&
        beginCall->getMethodDecl()->getNameAsString() == "data" && endBase->getMethodDecl()->getNameAsString() == "data" &&
        variable(beginCall->getImplicitObjectArgument()) == first.Owner && variable(endBase->getImplicitObjectArgument()) == first.Owner) ordered = true;
    // Require an ordered range starting at its base. Common allocation alone
    // does not establish valid arguments (for example from_chars(a+3,a+1,...)).
    if (!ordered) return {};
    return {first.Owner, false};
  }
  Position position(const clang::Expr *e, const clang::Expr *use, unsigned depth) {
    if (!e || depth > 20 || e->isTypeDependent()) return {};
    e = e->IgnoreParenImpCasts();
    if (const auto *cast = clang::dyn_cast<clang::ExplicitCastExpr>(e)) {
      if (cast->getCastKind() != clang::CK_NoOp) return {};
      return position(cast->getSubExpr(), use, depth + 1);
    }
    if (const auto *ref = clang::dyn_cast<clang::DeclRefExpr>(e)) {
      if (const auto *binding = clang::dyn_cast<clang::BindingDecl>(ref->getDecl())) {
        const auto *member = clang::dyn_cast_or_null<clang::MemberExpr>(binding->getBinding());
        const auto *owner = clang::dyn_cast_or_null<clang::VarDecl>(binding->getDecomposedDecl());
        if (member && member->getMemberDecl()->getNameAsString() == "ptr" && owner && owner->hasInit()) {
          Writes writes(owner);
          const auto *f = function(owner);
          if (!f || !f->hasBody()) return {};
          writes.TraverseStmt(const_cast<clang::Stmt *>(f->getBody()));
          class BindingUses : public clang::RecursiveASTVisitor<BindingUses> {
           public:
            clang::ASTContext &C; const clang::BindingDecl *B; bool Unknown = false;
            BindingUses(clang::ASTContext &c, const clang::BindingDecl *b) : C(c), B(b) {}
            bool VisitDeclRefExpr(clang::DeclRefExpr *r) {
              if (r->getDecl() != B) return true;
              auto parents = C.getParents(*r);
              const auto *read = parents.size() == 1 ? parents[0].get<clang::ImplicitCastExpr>() : nullptr;
              if (!read || read->getCastKind() != clang::CK_LValueToRValue) Unknown = true;
              return true;
            }
          } uses(C, binding);
          uses.TraverseStmt(const_cast<clang::Stmt *>(f->getBody()));
          if (writes.Unknown || uses.Unknown) return {};
          return fromChars(owner->getInit(), use, depth + 1);
        }
      }
    }
    if (const auto *v = variable(e)) {
      if (v->getType()->isConstantArrayType() && !v->getType()->isReferenceType()) return {v, true};
      if (!v->hasLocalStorage() || !v->hasInit() || !Active.insert(v).second) return {};
      auto result = position(v->getInit(), use, depth + 1);
      bool searched = false;
      // A successful search is in the source array, but NULL must be excluded.
      const auto *init = v->getInit()->IgnoreParenCasts();
      if (!result && v->getType()->isPointerType() && v->getType()->getPointeeType()->isCharType() && nonnull(v, use)) {
        if (const auto *call = clang::dyn_cast<clang::CallExpr>(init)) {
          const auto *callee = call->getDirectCallee();
          if (callee && callee->getDeclContext()->getRedeclContext()->isTranslationUnit() && C.getSourceManager().isInSystemHeader(callee->getLocation()) &&
              callee->getName() == "memchr" && call->getNumArgs() == 3) {
            result = position(call->getArg(0), use, depth + 1);
            const auto *size = clang::dyn_cast<clang::CXXMemberCallExpr>(call->getArg(2)->IgnoreParenImpCasts());
            bool bounded = result && size && size->getMethodDecl() &&
                size->getMethodDecl()->getNameAsString() == "size" &&
                standard(size->getMethodDecl()->getDeclContext()) &&
                variable(size->getImplicitObjectArgument()) == result.Owner;
            if (!bounded && result) {
              auto *array = C.getAsConstantArrayType(result.Owner->getType());
              auto length = call->getArg(2)->getIntegerConstantExpr(C);
              bounded = array && variable(call->getArg(0)) == result.Owner && length && !length->isNegative() &&
                  length->getLimitedValue() <= static_cast<uint64_t>(C.getTypeSizeInChars(result.Owner->getType()).getQuantity());
            }
            if (bounded) { result.Interior = true; searched = true; } else result = {};
          } else if (callee && callee->getIdentifier() && C.getSourceManager().isInSystemHeader(callee->getLocation()) &&
                     callee->getDeclContext()->getRedeclContext()->isTranslationUnit() &&
                     (callee->getName() == "strchr" || callee->getName() == "strrchr") && call->getNumArgs() == 2) {
            const auto *array = variable(call->getArg(0));
            if (terminatedArray(array)) { result = {array, true}; searched = true; }
          }
        }
      }
      if (result) {
        Writes writes(v);
        auto *f = function(v);
        if (!f || !f->hasBody()) result = {};
        else {
          writes.TraverseStmt(const_cast<clang::Stmt *>(f->getBody()));
          if (writes.Unknown || writes.Increments.size() > 1) result = {};
          else for (auto *inc : writes.Increments)
            if ((searched && !nonnull(v, inc)) || !boundedIncrement(inc, v, result, use, depth + 1)) result = {};
          if (!writes.Increments.empty()) result.Interior = false;
        }
      }
      Active.erase(v);
      return result;
    }
    if (const auto *call = clang::dyn_cast<clang::CXXMemberCallExpr>(e)) {
      const auto *method = call->getMethodDecl();
      const auto *owner = variable(call->getImplicitObjectArgument());
      if (!method || !owner || method->getNameAsString() != "data" || !standard(method->getDeclContext())) return {};
      const auto name = method->getParent()->getName();
      if (name != "array" && name != "basic_string_view") return {};
      if (!owner->hasLocalStorage()) return {};
      if (owner->getType()->isReferenceType()) {
        if (!clang::isa<clang::ParmVarDecl>(owner) || !owner->getType()->getPointeeType().isConstQualified()) return {};
        ReferenceEffects effects(C);
        const auto *f = function(owner);
        if (!f || !f->hasBody()) return {};
        effects.TraverseStmt(const_cast<clang::Stmt *>(f->getBody()));
        if (effects.Unknown) return {};
      }
      Writes writes(owner);
      auto *f = function(owner);
      if (!f || !f->hasBody()) return {};
      writes.TraverseStmt(const_cast<clang::Stmt *>(f->getBody()));
      if (writes.Unknown || !writes.Increments.empty()) return {};
      // Standard view/array accessors preserve the range relation. Backing
      // storage lifetime remains a separate obligation, including for views.
      return {owner, false};
    }
    if (const auto *op = clang::dyn_cast<clang::BinaryOperator>(e)) {
      if (op->getOpcode() != clang::BO_Add) return {};
      auto base = position(op->getLHS(), use, depth + 1);
      if (!base) return {};
      if (auto value = op->getRHS()->getIntegerConstantExpr(C)) {
        if (value->isZero()) return base;
        if (variable(op->getLHS()) == base.Owner) {
          auto *array = C.getAsConstantArrayType(base.Owner->getType());
          if (array && !value->isNegative() && value->getLimitedValue() <= array->getSize().getLimitedValue())
            return {base.Owner, value->getLimitedValue() < array->getSize().getLimitedValue()};
        }
        if (value->isOne() && base.Interior) return {base.Owner, false};
      }
      const auto *size = clang::dyn_cast<clang::CXXMemberCallExpr>(op->getRHS()->IgnoreParenImpCasts());
      const auto *data = clang::dyn_cast<clang::CXXMemberCallExpr>(op->getLHS()->IgnoreParenImpCasts());
      if (data && size && size->getMethodDecl() && size->getMethodDecl()->getNameAsString() == "size" &&
          standard(size->getMethodDecl()->getDeclContext()) && variable(size->getImplicitObjectArgument()) == base.Owner)
        return {base.Owner, false};
    }
    return {};
  }
};
} // namespace baseline_pointer
