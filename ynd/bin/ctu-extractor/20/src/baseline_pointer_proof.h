#pragma once

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/Basic/SourceManager.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/SaveAndRestore.h"

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
  // Reuse the same write/escape validation and dominating-guard bounds as
  // arithmetic safety proofs. Parameter reads, stable local copies and literal
  // endpoints enter this interface; arbitrary constexpr calls are not ranges.
  bool invariantComparison(const clang::BinaryOperator *e) {
    if (!e || !e->isComparisonOp()) return false;
    if (!positiveRangeOperand(e->getLHS()) || !positiveRangeOperand(e->getRHS())) return false;
    llvm::SaveAndRestore<bool> positive(PositiveRangesOnly, true);
    llvm::APInt alo(128, 0), ahi(128, 0), blo(128, 0), bhi(128, 0);
    if (!stableRange(e->getLHS(), e, alo, ahi) || !stableRange(e->getRHS(), e, blo, bhi)) return false;
    switch (e->getOpcode()) {
      case clang::BO_LT: return ahi.slt(blo) || alo.sge(bhi);
      case clang::BO_LE: return ahi.sle(blo) || alo.sgt(bhi);
      case clang::BO_GT: return alo.sgt(bhi) || ahi.sle(blo);
      case clang::BO_GE: return alo.sge(bhi) || ahi.slt(blo);
      case clang::BO_EQ: case clang::BO_NE:
        return ahi.slt(blo) || alo.sgt(bhi) || (alo == ahi && blo == bhi && alo == blo);
      default: return false;
    }
  }
  // A violation requires the entire offset interval outside [0, extent].
  // One-past is a valid pointer value; uncertain/mixed intervals are not proof.
  const clang::VarDecl *outsideArrayArithmetic(const clang::BinaryOperator *e) {
    if (!e || !e->getType()->isPointerType() ||
        (e->getOpcode()!=clang::BO_Add && e->getOpcode()!=clang::BO_Sub)) return nullptr;
    const clang::Expr *base=e->getLHS(), *offset=e->getRHS();
    if(e->getOpcode()==clang::BO_Add && base->getType()->isIntegerType()) std::swap(base,offset);
    const auto *owner=variable(base);
    // Match the existing literal witness: automatic complete arrays have
    // unambiguous object identity without linker alias/interposition evidence.
    if (!owner || !owner->hasLocalStorage()) return nullptr;
    const auto *array=C.getAsConstantArrayType(owner->getType());
    if(!array || array->getSize().getActiveBits()>127 || !positiveRangeOperand(offset)) return nullptr;
    llvm::SaveAndRestore<bool> positive(PositiveRangesOnly,true);
    llvm::APInt lo(128,0), hi(128,0);
    if(!stableRange(offset,e,lo,hi)) return nullptr;
    if(e->getOpcode()==clang::BO_Sub) {const auto oldLo=lo;lo=-hi;hi=-oldLo;}
    const auto extent=array->getSize().zextOrTrunc(128);
    return hi.isNegative() || lo.sgt(extent) ? owner : nullptr;
  }
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
    if (!e->isIncrementDecrementOp() || !e->getType()->isSignedIntegerType()) return false;
    const auto *v = variable(e->getSubExpr());
    if (!v || !v->hasLocalStorage() || v->getType()->isReferenceType() ||
        v->getType().isVolatileQualified()) return false;
    const auto *owner = function(v);
    if (!owner || !owner->hasBody()) return false;
    // A local address/reference escape permits a callee to change the counter
    // between guard and increment. Scan the entire function, including bindings
    // made before the loop; checking only writes inside the loop is insufficient.
    Writes escapes(v);
    escapes.EscapesOnly = true;
    escapes.TraverseStmt(const_cast<clang::Stmt *>(owner->getBody()));
    if (escapes.Unknown) return false;
    auto node = clang::DynTypedNode::create(*e);
    for (unsigned depth = 0; depth < 12; ++depth) {
      auto parents = C.getParents(node); if (parents.size() != 1) return false;
      node = parents[0];
      const auto *loop = node.get<clang::ForStmt>();
      if (!loop) continue;
      if (!loop->getCond() || !loop->getInc() || loop->getInc()->IgnoreParenImpCasts() != e) return false;
      const auto *condition = clang::dyn_cast<clang::BinaryOperator>(loop->getCond()->IgnoreParenImpCasts());
      if (!condition || variable(condition->getLHS()) != v ||
          !condition->getLHS()->getType()->isSignedIntegerType() ||
          !condition->getRHS()->getType()->isSignedIntegerType()) return false;
      const bool increment = e->isIncrementOp();
      const auto opcode = condition->getOpcode();
      const bool strict = opcode == (increment ? clang::BO_LT : clang::BO_GT);
      if (!strict && opcode != (increment ? clang::BO_LE : clang::BO_GE)) return false;
      const unsigned bits = C.getIntWidth(v->getType());
      const auto *bound = condition->getRHS();
      bool bounded = strict && C.getIntWidth(bound->getType()) <= bits;
      if (auto constant = bound->getIntegerConstantExpr(C)) {
        // Signed comparisons preserve the counter value, even after promotion.
        const auto wide = constant->sextOrTrunc(std::max(bits, constant->getBitWidth()) + 1);
        const auto limit = (increment ? llvm::APInt::getSignedMaxValue(bits) :
                                       llvm::APInt::getSignedMinValue(bits)).sext(wide.getBitWidth());
        bounded = increment ? (strict ? wide.sle(limit) : wide.slt(limit)) :
                              (strict ? wide.sge(limit) : wide.sgt(limit));
      } else if (!strict) {
        // For a defined signed n-k (k>0), the result is below the signed maximum.
        // The subtraction itself retains its independent overflow obligation.
        const auto *offset = clang::dyn_cast<clang::BinaryOperator>(bound->IgnoreParenImpCasts());
        if (offset && offset->getOpcode() == (increment ? clang::BO_Sub : clang::BO_Add) &&
            offset->getType()->isSignedIntegerType() && C.getIntWidth(offset->getType()) <= bits) {
          auto amount = offset->getRHS()->getIntegerConstantExpr(C);
          bounded = amount && amount->isStrictlyPositive();
        }
      }
      if (!bounded) return false;
      Writes writes(v);
      writes.TraverseStmt(const_cast<clang::Expr *>(loop->getCond()));
      writes.TraverseStmt(const_cast<clang::Stmt *>(loop->getBody()));
      return !writes.Unknown && writes.Increments.empty();
    }
    return false;
  }
  // Signed arithmetic on stable scalar inputs, narrowed only by structural
  // dominating guards. Unknown calls cannot mutate an unescaped automatic
  // scalar; nonlocal, volatile, aliased and assigned inputs remain unknown.
  bool safeSignedArithmetic(const clang::BinaryOperator *e) {
    llvm::APInt lo(128, 0), hi(128, 0);
    if (!signedArithmeticRange(e, lo, hi)) return false;
    const unsigned bits = C.getIntWidth(e->getType());
    return lo.sge(llvm::APInt::getSignedMinValue(bits).sext(128)) &&
        hi.sle(llvm::APInt::getSignedMaxValue(bits).sext(128));
  }
  // A failed safety proof is not an overflow proof. Every possible result must
  // be outside the representable interval. Keep runtime-stable inputs only.
  bool overflowingSignedArithmetic(const clang::BinaryOperator *e) {
    if (!e || !positiveRangeOperand(e->getLHS()) || !positiveRangeOperand(e->getRHS())) return false;
    llvm::SaveAndRestore<bool> positive(PositiveRangesOnly, true);
    llvm::APInt lo(128, 0), hi(128, 0);
    if (!signedArithmeticRange(e, lo, hi)) return false;
    const unsigned bits = C.getIntWidth(e->getType());
    return hi.slt(llvm::APInt::getSignedMinValue(bits).sext(128)) ||
        lo.sgt(llvm::APInt::getSignedMaxValue(bits).sext(128));
  }
  // Reuse stable operand ranges and their write/escape checks. These are
  // positive invalid-operation predicates, never negated safety predicates.
  bool zeroDivisor(const clang::BinaryOperator *e) {
    if (!e || (e->getOpcode()!=clang::BO_Div && e->getOpcode()!=clang::BO_Rem) ||
        !positiveRangeOperand(e->getLHS()) || !positiveRangeOperand(e->getRHS())) return false;
    llvm::SaveAndRestore<bool> positive(PositiveRangesOnly,true);
    llvm::APInt alo(128,0),ahi(128,0),blo(128,0),bhi(128,0);
    return stableRange(e->getLHS(),e,alo,ahi) && stableRange(e->getRHS(),e,blo,bhi) &&
        blo.isZero() && bhi.isZero();
  }
  bool invalidShiftCount(const clang::BinaryOperator *e) {
    if (!e || (e->getOpcode()!=clang::BO_Shl && e->getOpcode()!=clang::BO_Shr) ||
        !positiveRangeOperand(e->getLHS()) || !positiveRangeOperand(e->getRHS())) return false;
    llvm::SaveAndRestore<bool> positive(PositiveRangesOnly,true);
    llvm::APInt alo(128,0),ahi(128,0),blo(128,0),bhi(128,0);
    if (!stableRange(e->getLHS(),e,alo,ahi) || !stableRange(e->getRHS(),e,blo,bhi)) return false;
    // Clang's operand type already includes integral promotions. A narrow
    // source integer is shifted at the promoted width, not its storage width.
    return bhi.isNegative() || blo.sge(llvm::APInt(128,C.getIntWidth(e->getLHS()->getType())));
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
  static bool positiveRangeOperand(const clang::Expr *value, unsigned depth = 0) {
    if (!value || depth > 16) return false;
    value = value->IgnoreParenImpCasts();
    if (clang::isa<clang::IntegerLiteral>(value)) return true;
    if (const auto *u = clang::dyn_cast<clang::UnaryOperator>(value))
      return (u->getOpcode() == clang::UO_Minus || u->getOpcode() == clang::UO_Plus) &&
             clang::isa<clang::IntegerLiteral>(u->getSubExpr()->IgnoreParenImpCasts());
    const auto *ref = clang::dyn_cast<clang::DeclRefExpr>(value);
    const auto *variable = ref ? clang::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
    if (!variable || !variable->hasLocalStorage() || variable->getType().isVolatileQualified()) return false;
    return clang::isa<clang::ParmVarDecl>(variable) ||
        (variable->hasInit() && positiveRangeOperand(variable->getInit(), depth + 1));
  }
  bool signedArithmeticRange(const clang::BinaryOperator *e, llvm::APInt &lo, llvm::APInt &hi) {
    if (!e || !e->getType()->isSignedIntegerType() || C.getIntWidth(e->getType()) > 64 ||
        (e->getOpcode() != clang::BO_Add && e->getOpcode() != clang::BO_Sub &&
         e->getOpcode() != clang::BO_Mul)) return false;
    llvm::APInt alo(128, 0), ahi(128, 0), blo(128, 0), bhi(128, 0);
    if (!stableRange(e->getLHS(), e, alo, ahi) || !stableRange(e->getRHS(), e, blo, bhi)) return false;
    if (e->getOpcode() == clang::BO_Add) { lo = alo + blo; hi = ahi + bhi; }
    else if (e->getOpcode() == clang::BO_Sub) { lo = alo - bhi; hi = ahi - blo; }
    else {
      lo = hi = alo * blo;
      for (const auto &product : {alo * bhi, ahi * blo, ahi * bhi}) {
        if (product.slt(lo)) lo = product;
        if (product.sgt(hi)) hi = product;
      }
    }
    return true;
  }
  bool PositiveRangesOnly = false;
  static bool literalBound(const clang::Expr *expression) {
    expression = expression->IgnoreParenImpCasts();
    if (clang::isa<clang::IntegerLiteral>(expression)) return true;
    const auto *unary = clang::dyn_cast<clang::UnaryOperator>(expression);
    return unary && (unary->getOpcode() == clang::UO_Minus || unary->getOpcode() == clang::UO_Plus) &&
           clang::isa<clang::IntegerLiteral>(unary->getSubExpr()->IgnoreParenImpCasts());
  }
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
      if (!v->hasAttr<clang::CleanupAttr>() && callee && callee->getIdentifier() && callee->getName() == "readlink" && callee->isExternC() &&
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
          // Check the converted operands: len <= 0ULL converts a negative signed
          // result to unsigned and does not establish a nonnegative len.
          if (!condition->getLHS()->getType()->isSignedIntegerType() ||
              !condition->getRHS()->getType()->isSignedIntegerType()) continue;
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
  bool preservingSignedRead(const clang::Expr *value, const clang::VarDecl *v) {
    value = value->IgnoreParens();
    for (unsigned depth = 0; depth != 16; ++depth) {
      const auto *cast = clang::dyn_cast<clang::ImplicitCastExpr>(value);
      if (!cast) return variable(value) == v;
      if (cast->getCastKind() != clang::CK_LValueToRValue && cast->getCastKind() != clang::CK_IntegralCast &&
          cast->getCastKind() != clang::CK_NoOp) return false;
      if (!cast->getType()->isSignedIntegerType() || !cast->getSubExpr()->getType()->isSignedIntegerType() ||
          C.getIntWidth(cast->getType()) < C.getIntWidth(cast->getSubExpr()->getType())) return false;
      value = cast->getSubExpr()->IgnoreParens();
    }
    return false;
  }
  void narrowStable(const clang::Expr *condition, bool truth, const clang::VarDecl *v,
                    llvm::APInt &lo, llvm::APInt &hi, unsigned depth = 0) {
    if (!condition || depth > 16) return;
    condition = condition->IgnoreParenImpCasts();
    if (const auto *negation = clang::dyn_cast<clang::UnaryOperator>(condition)) {
      if (negation->getOpcode() == clang::UO_LNot)
        narrowStable(negation->getSubExpr(), !truth, v, lo, hi, depth + 1);
      return;
    }
    const auto *test = clang::dyn_cast<clang::BinaryOperator>(condition);
    if (!test) return;
    if ((test->getOpcode() == clang::BO_LAnd && truth) ||
        (test->getOpcode() == clang::BO_LOr && !truth)) {
      narrowStable(test->getLHS(), truth, v, lo, hi, depth + 1);
      narrowStable(test->getRHS(), truth, v, lo, hi, depth + 1);
      return;
    }
    if (!test->isComparisonOp() || !test->getLHS()->getType()->isSignedIntegerType() ||
        !test->getRHS()->getType()->isSignedIntegerType()) return;
    auto opcode = test->getOpcode();
    const clang::Expr *value = test->getLHS(), *bound = test->getRHS();
    if (variable(value) != v) {
      std::swap(value, bound);
      opcode = clang::BinaryOperator::reverseComparisonOp(opcode);
    }
    // Ignore only value-preserving signed promotions, never narrowing casts.
    if (!preservingSignedRead(value, v) || !C.hasSameUnqualifiedType(value->IgnoreParenImpCasts()->getType(), v->getType()) ||
        C.getIntWidth(value->getType()) < C.getIntWidth(v->getType())) return;
    if (PositiveRangesOnly && !literalBound(bound)) return;
    const auto constant = bound->getIntegerConstantExpr(C);
    if (!constant || constant->getBitWidth() > 64) return;
    const auto limit = constant->isUnsigned() ? constant->zextOrTrunc(128) : constant->sextOrTrunc(128);
    if (!truth) opcode = clang::BinaryOperator::negateComparisonOp(opcode);
    auto lower = limit, upper = limit;
    switch (opcode) {
      case clang::BO_GT: lower += 1; [[fallthrough]];
      case clang::BO_GE: if (lower.sgt(lo)) lo = lower; break;
      case clang::BO_LT: upper -= 1; [[fallthrough]];
      case clang::BO_LE: if (upper.slt(hi)) hi = upper; break;
      case clang::BO_EQ: if (limit.sgt(lo)) lo = limit; if (limit.slt(hi)) hi = limit; break;
      default: break;
    }
  }
  bool loopCounterRange(const clang::VarDecl *v, const clang::Expr *use,
                        llvm::APInt &lo, llvm::APInt &hi) {
    const clang::Stmt *node = use;
    for (unsigned depth = 0; node && depth != 64; ++depth) {
      const auto parents = C.getParents(*node);
      if (parents.size() != 1) return false;
      const auto *parent = parents[0].get<clang::Stmt>();
      if (!parent) return false;
      const auto *loop = clang::dyn_cast<clang::ForStmt>(parent);
      if (loop && node == loop->getBody() && loop->getCond() && loop->getInc()) {
        const auto *step = clang::dyn_cast<clang::UnaryOperator>(loop->getInc()->IgnoreParenImpCasts());
        const auto *test = clang::dyn_cast<clang::BinaryOperator>(loop->getCond()->IgnoreParenImpCasts());
        if (!step || !step->isIncrementOp() || variable(step->getSubExpr()) != v ||
            !test || variable(test->getLHS()) != v || !preservingSignedRead(test->getLHS(), v) ||
            !test->getRHS()->getType()->isSignedIntegerType() ||
            C.getIntWidth(test->getRHS()->getType()) > C.getIntWidth(v->getType()) ||
            (test->getOpcode() != clang::BO_LT && test->getOpcode() != clang::BO_LE)) return false;
        const clang::Expr *initial = nullptr;
        if (const auto *assignment = clang::dyn_cast_or_null<clang::BinaryOperator>(loop->getInit())) {
          if (assignment->getOpcode() == clang::BO_Assign && variable(assignment->getLHS()) == v)
            initial = assignment->getRHS();
        } else if (const auto *declaration = clang::dyn_cast_or_null<clang::DeclStmt>(loop->getInit())) {
          if (declaration->isSingleDecl() && declaration->getSingleDecl() == v) initial = v->getInit();
        }
        const auto start = initial ? initial->getIntegerConstantExpr(C) : std::nullopt;
        if (!start || start->getBitWidth() > 64) return false;
        Writes body(v); body.TraverseStmt(const_cast<clang::Stmt *>(loop->getBody()));
        body.TraverseStmt(const_cast<clang::Expr *>(loop->getCond()));
        if (body.Unknown || !body.Increments.empty()) return false;
        const unsigned bits = C.getIntWidth(v->getType());
        lo = start->isUnsigned() ? start->zextOrTrunc(128) : start->sextOrTrunc(128);
        const auto minimum = llvm::APInt::getSignedMinValue(bits).sext(128);
        const auto maximum = llvm::APInt::getSignedMaxValue(bits).sext(128);
        if (lo.slt(minimum) || lo.sgt(maximum)) return false;
        hi = maximum;
        if (test->getOpcode() == clang::BO_LT) hi -= 1;
        else {
          // As for safeIntegerIncrement, evaluating n-k has its own overflow
          // obligation. A defined signed result with k>0 is below the maximum.
          const auto *offset = clang::dyn_cast<clang::BinaryOperator>(test->getRHS()->IgnoreParenImpCasts());
          const auto amount = offset && offset->getOpcode() == clang::BO_Sub
              ? offset->getRHS()->getIntegerConstantExpr(C) : std::nullopt;
          if (amount && amount->isStrictlyPositive()) hi -= 1;
          else if (auto bound = test->getRHS()->getIntegerConstantExpr(C)) {
            hi = bound->sextOrTrunc(128);
            if (hi.sge(maximum)) return false;
          } else return false;
        }
        return lo.sle(hi);
      }
      node = parent;
    }
    return false;
  }
  bool stableRange(const clang::Expr *value, const clang::Expr *use,
                   llvm::APInt &lo, llvm::APInt &hi, unsigned depth = 0) {
    if (depth > 16) return false;
    if (!value->getType()->isSignedIntegerType() || C.getIntWidth(value->getType()) > 64) return false;
    if (auto constant = value->getIntegerConstantExpr(C)) {
      lo = hi = constant->sextOrTrunc(128); return true;
    }
    const auto *v = variable(value);
    if (!v || !preservingSignedRead(value, v) || !v->hasLocalStorage() || !v->getType()->isSignedIntegerType() ||
        v->getType().isVolatileQualified() || C.getIntWidth(v->getType()) > C.getIntWidth(value->getType())) return false;
    const auto *fn = function(v);
    if (!fn || !fn->hasBody()) return false;
    Writes escaped(v); escaped.EscapesOnly = true; escaped.TraverseStmt(fn->getBody());
    Writes written(v); written.TraverseStmt(fn->getBody());
    if (escaped.Unknown) return false;
    if (written.Unknown || !written.Increments.empty())
      return !PositiveRangesOnly && loopCounterRange(v, use, lo, hi);
    lo = llvm::APInt::getSignedMinValue(C.getIntWidth(v->getType())).sext(128);
    hi = llvm::APInt::getSignedMaxValue(C.getIntWidth(v->getType())).sext(128);
    // A stable local copy has the same value as its initializer. Reuse the
    // range transfer recursively only when the conversion preserves the full
    // interval; narrowing/wrapping conversions are not identity transfers.
    if (v->hasInit() && positiveRangeOperand(v->getInit())) {
      llvm::APInt initialLo(128, 0), initialHi(128, 0);
      if (stableRange(v->getInit(), use, initialLo, initialHi, depth + 1) &&
          initialLo.sge(lo) && initialHi.sle(hi)) {
        lo = initialLo; hi = initialHi;
      }
    }
    const clang::Stmt *node = use;
    for (unsigned depth = 0; node && depth != 64; ++depth) {
      const auto parents = C.getParents(*node);
      if (parents.size() != 1) break;
      const auto *parent = parents[0].get<clang::Stmt>();
      if (!parent) break;
      if (const auto *test = clang::dyn_cast<clang::IfStmt>(parent)) {
        if (node == test->getThen()) narrowStable(test->getCond(), true, v, lo, hi);
        if (node == test->getElse()) narrowStable(test->getCond(), false, v, lo, hi);
      }
      if (const auto *binary = clang::dyn_cast<clang::BinaryOperator>(parent))
        if (node == binary->getRHS() && binary->isLogicalOp())
          narrowStable(binary->getLHS(), binary->getOpcode() == clang::BO_LAnd, v, lo, hi);
      if (const auto *scope = clang::dyn_cast<clang::CompoundStmt>(parent)) {
        for (const auto *previous : scope->body()) {
          if (previous == node) break;
          const auto *test = clang::dyn_cast<clang::IfStmt>(previous);
          if (!test) continue;
          if (exits(test->getThen())) narrowStable(test->getCond(), false, v, lo, hi);
          if (exits(test->getElse())) narrowStable(test->getCond(), true, v, lo, hi);
        }
      }
      node = parent;
    }
    return lo.sle(hi);
  }
  class Writes : public clang::RecursiveASTVisitor<Writes> {
   public:
    bool shouldVisitImplicitCode() const { return true; }
    const clang::VarDecl *V;
    bool Unknown = false;
    bool EscapesOnly = false;
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
    bool VisitCXXParenListInitExpr(clang::CXXParenListInitExpr *e) {
      for (const auto *init : e->getInitExprs())
        if (init->isGLValue() && aliases(init)) Unknown = true;
      return true;
    }
    bool VisitLambdaExpr(clang::LambdaExpr *e) {
      if (EscapesOnly && captures(e)) Unknown = true;
      return true;
    }
    bool VisitCoroutineBodyStmt(clang::CoroutineBodyStmt *) {
      if (EscapesOnly) Unknown = true;
      return true;
    }
    bool VisitExplicitCastExpr(clang::ExplicitCastExpr *e) {
      if (e->isGLValue() && aliases(e)) Unknown = true;
      return true;
    }
    bool VisitBinaryOperator(clang::BinaryOperator *e) {
      if (!EscapesOnly && e->isAssignmentOp() && aliases(e->getLHS())) {
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
      if (!EscapesOnly && e->isIncrementDecrementOp()) {
        if (e->isIncrementOp()) Increments.push_back(e); else Unknown = true;
      }
      return true;
    }
    bool VisitVarDecl(clang::VarDecl *d) {
      if (d != IgnoredBinding && d->getType()->isReferenceType() && aliases(d->getInit())) Unknown = true;
      return true;
    }
    bool VisitCallExpr(clang::CallExpr *e) {
      if (EscapesOnly) {
        const auto *callee = e->getDirectCallee();
        if (callee && (callee->hasAttr<clang::ReturnsTwiceAttr>() ||
            callee->getNameAsString().find("setjmp") != std::string::npos)) Unknown = true;
      }

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
    bool VisitCaseStmt(clang::CaseStmt *) { Unknown = true; return true; }
    bool VisitDefaultStmt(clang::DefaultStmt *) { Unknown = true; return true; }
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
