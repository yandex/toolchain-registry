#include "initialization_flow.h"
#include "baseline.h"

#include <limits>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "clang/AST/Attr.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Analysis/CFG.h"

namespace {
using Value = facts_pb::InitializationValue;
using Operation = facts_pb::InitializationOperation;
std::map<const Expr *, std::pair<facts_pb::EntityId, uint64_t>> anchors;
std::set<const Expr *> ambiguousAnchors;
std::map<const Expr *, std::string> readFailures;

// Emit explicit fallback bodies, so cross-TU definitions can override contracts.
std::set<const FunctionDecl *> mathContracts;
void emitMathContract(const FunctionDecl *callee, ASTContext &ctx) {
  if (!hasDefinedMathReturn(callee, ctx) ||
      !mathContracts.insert(callee->getCanonicalDecl()).second) return;
  facts_pb::Fact fact;
  fact.set_origin(originOf(true));
  auto *fn = fact.mutable_initialization_function();
  computeFuncEntityId(const_cast<FunctionDecl *>(callee), ctx,
                      ctx.getSourceManager(), fn->mutable_function());
  fn->set_library_return_contract("math-initialized-v1:" +
      callee->getType().getCanonicalType().getAsString());
  fn->set_parameter_count(callee->getNumParams());
  fn->set_version(8);
  fn->set_complete(true);
  fn->set_scalar_return(true);
  auto *block = fn->add_blocks();
  block->set_is_entry(true);
  auto *op = block->add_operations();
  op->set_kind(Operation::IO_RETURN);
  op->mutable_value()->set_kind(Value::IV_DEFINED);
  g_facts.push_back(std::move(fact));
}

struct CallerCensus {
  bool escaped = false;
  std::map<const FunctionDecl *, unsigned> callers;
};
std::map<const FunctionDecl *, CallerCensus> callerCensus;
bool censusOpaque = false;
class CallerVisitor : public RecursiveASTVisitor<CallerVisitor> {
 public:
  explicit CallerVisitor(ASTContext &context) : context(context) {}
  bool TraverseFunctionDecl(FunctionDecl *function) {
    const auto *previous = owner;
    owner = function;
    bool result = RecursiveASTVisitor<CallerVisitor>::TraverseFunctionDecl(function);
    owner = previous;
    return result;
  }
  bool VisitAsmStmt(AsmStmt *) { censusOpaque = true; return true; }
  bool VisitFileScopeAsmDecl(FileScopeAsmDecl *) { censusOpaque = true; return true; }
  bool VisitDeclRefExpr(DeclRefExpr *ref) {
    const auto *fn = dyn_cast<FunctionDecl>(ref->getDecl());
    if (!fn || fn->isExternallyVisible() || isa<CXXMethodDecl>(fn) ||
        fn->isVariadic() || fn->hasAttrs() || fn->getTemplatedKind() || censusOpaque)
      return true;
    if (++references > 262144 || callerCensus.size() >= 65536) {
      censusOpaque = true;
      return true;
    }
    auto &census = callerCensus[fn->getCanonicalDecl()];
    if (census.escaped) return true;
    const Expr *expr = ref;
    for (unsigned depth = 0; depth < 32; ++depth) {
      auto parents = context.getParents(*expr);
      if (parents.size() != 1) break;
      if (const auto *cast = parents[0].get<ImplicitCastExpr>()) { expr = cast; continue; }
      if (const auto *paren = parents[0].get<ParenExpr>()) { expr = paren; continue; }
      if (const auto *call = parents[0].get<CallExpr>()) {
        if (owner && !owner->isDependentContext() && !owner->getTemplatedKind() &&
            call->getCallee() == expr && call->getNumArgs() == fn->getNumParams()) {
          if (census.callers.size() >= 256 ||
              ++census.callers[owner->getCanonicalDecl()] > 16384) {
            census.escaped = true;
            census.callers.clear();
          }
          return true;
        }
      }
      break;
    }
    census.escaped = true;
    return true;
  }
 private:
  ASTContext &context;
  const FunctionDecl *owner = nullptr;
  unsigned references = 0;
};

bool tracked(QualType type) {
  return !type.isNull() && !type.isVolatileQualified() &&
         (type->isIntegerType() || type->isRealFloatingType() ||
          type->isEnumeralType());
}

// Volatile affects observability and value stability, not whether a private
// automatic scalar has received an initialization. This predicate is used only
// by the must-initialization storage/transfer channel. Effect isolation keeps
// using tracked(), and escapes poison volatile storage just like other locals.
bool initializationScalar(QualType type) {
  return !type.isNull() && (tracked(type.getUnqualifiedType()) || type->isPointerType());
}

// A separate may-effect pass: accepting an expression here never supplies an
// initialization or lifetime proof. Unsupported escape operations invalidate the
// entire certificate instead of assuming a borrowed pointer cannot escape.
class BorrowEffects : public RecursiveASTVisitor<BorrowEffects> {
 public:
  BorrowEffects(FunctionDecl *fn, ASTContext &ctx,
                facts_pb::BorrowedEffects *out) : ctx(ctx), out(out) {
    out->set_version(2);
    complete = !isa<CXXMethodDecl>(fn) && !fn->isVariadic() &&
               !fn->isDependentContext() &&
               (tracked(fn->getReturnType()) || fn->getReturnType()->isVoidType() || fn->getReturnType()->isPointerType() || scalarBorrowedType(fn->getReturnType()));
    out->set_returns_borrowed(scalarBorrowedType(fn->getReturnType()));
    for (unsigned i = 0; i < fn->getNumParams(); ++i) {
      auto *p = fn->getParamDecl(i);
      parameters[p] = i;
      bool borrowed = borrowedType(p->getType());
      out->add_borrowed_parameters(borrowed);
      out->add_parameter_effects(0);
      if (!borrowed && !tracked(p->getType())) complete = false;
      // Returned-borrow certificates currently identify an entire scalar
      // formal, not a subobject path. Never reuse a record-field effect binding
      // as an exact returned-pointer binding.
      if (borrowed && !scalarBorrowedType(p->getType()) && out->returns_borrowed()) complete = false;
    }
    if (complete) TraverseStmt(fn->getBody());
    for (const auto *e : returns) {
      auto *origin = out->add_returns();
      if (const auto *call = dyn_cast_or_null<CallExpr>(e)) {
        auto found = callIndices.find(call);
        if (found == callIndices.end()) { complete = false; continue; }
        origin->set_from_call(true);
        origin->set_call_index(found->second);
      } else {
        int index = -3;
        if (e && e->getType()->isPointerType()) {
          const auto *v = variable(e);
          auto found = parameters.find(v);
          if (found != parameters.end() && borrowedType(v->getType())) index = found->second;
          else if (const auto *address = dyn_cast<UnaryOperator>(e))
            if (address->getOpcode() == UO_AddrOf) index = storage(address->getSubExpr());
        } else index = storage(e);
        origin->set_parameter(index);
        if (index < 0) complete = false;
      }
    }
    out->set_complete(complete);
  }
  static bool scalarBorrowedType(QualType t) {
    return !t.isVolatileQualified() &&
           (t->isPointerType() || t->isLValueReferenceType()) &&
           tracked(t->getPointeeType());
  }
  static bool borrowedType(QualType t) {
    return scalarBorrowedType(t) || (!t.isVolatileQualified() &&
        (t->isPointerType() || t->isLValueReferenceType()) &&
        !t->getPointeeType().isVolatileQualified() &&
        (t->getPointeeType()->isRecordType() || t->getPointeeType()->isPointerType() || t->getPointeeType()->isVoidType()));
  }
  const VarDecl *variable(const Expr *e) {
    const auto *ref = e ? dyn_cast<DeclRefExpr>(e->IgnoreParenImpCasts()) : nullptr;
    return ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
  }
  static bool pointerCast(const CastExpr *e) {
    return e && e->getType()->isPointerType() && e->getSubExpr()->getType()->isPointerType() &&
        (e->getCastKind() == CK_NoOp || e->getCastKind() == CK_BitCast || e->getCastKind() == CK_LValueToRValue);
  }
  static bool pointerOffset(const BinaryOperator *e) {
    return e && (e->getOpcode() == BO_Add || e->getOpcode() == BO_Sub) &&
        e->getType()->isPointerType() && e->getLHS()->getType()->isPointerType() && e->getRHS()->getType()->isIntegerType();
  }
  // May-effect root only: offsets/casts do not establish exact alias, bounds,
  // lifetime, initialization, or a returned-borrow identity.
  int pointerRoot(const Expr *e, unsigned depth = 0) {
    if (!e || depth > 64) return -3;
    e = e->IgnoreParens();
    if (const auto *cast = dyn_cast<CastExpr>(e))
      return pointerCast(cast) ? pointerRoot(cast->getSubExpr(), depth + 1) : -3;
    if (const auto *offset = dyn_cast<BinaryOperator>(e))
      return pointerOffset(offset) ? pointerRoot(offset->getLHS(), depth + 1) : -3;
    const auto *v = variable(e);
    auto formal = parameters.find(v);
    if (formal != parameters.end() && v->getType()->isPointerType() && borrowedType(v->getType())) return formal->second;
    auto alias = pointerAliases.find(v);
    auto assigned = aliasAssignments.find(v);
    if (assigned != aliasAssignments.end() && !assignmentDominates(assigned->second, e)) return -3;
    return alias == pointerAliases.end() ? -3 : alias->second;
  }
  bool assignmentDominates(const BinaryOperator *assignment, const Expr *use) {
    auto parents = ctx.getParents(*assignment);
    const auto *scope = parents.size() == 1 ? parents[0].get<CompoundStmt>() : nullptr;
    if (!scope) return false;
    const Stmt *statement = use;
    for (unsigned depth = 0; depth != 64; ++depth) {
      auto path = ctx.getParents(*statement);
      if (path.size() != 1) return false;
      if (path[0].get<CompoundStmt>() == scope) break;
      statement = path[0].get<Stmt>();
      if (!statement || isa<SwitchCase>(statement)) return false;
    }
    bool foundAssignment = false;
    for (const auto *child : scope->body()) {
      if (isa<SwitchCase>(child)) return false;
      if (child == statement) return foundAssignment;
      if (child == assignment) foundAssignment = true;
    }
    return false;
  }
  bool bindDelayedPointer(const BinaryOperator *assignment) {
    const auto *v = variable(assignment->getLHS());
    if (!delayedPointers.count(v) || pointerAliases.count(v)) return false;
    auto parents = ctx.getParents(*assignment);
    if (parents.size() != 1 || !parents[0].get<CompoundStmt>()) return false;
    // A switch may enter a later case without passing the assignment.
    const Stmt *ancestor = assignment;
    for (unsigned depth = 0; depth != 64; ++depth) {
      auto path = ctx.getParents(*ancestor);
      if (path.size() != 1) return false;
      ancestor = path[0].get<Stmt>();
      if (!ancestor) break;
      if (isa<SwitchStmt>(ancestor)) return false;
      if (depth == 63) return false;
    }
    int root = pointerRoot(assignment->getRHS());
    if (root < 0) return false;
    pointerAliases[v] = root;
    aliasAssignments[v] = assignment;
    return true;
  }
  // -2 is private scalar storage; >=0 is a borrowed formal; -3 is unknown.
  int storage(const Expr *e, unsigned depth = 0) {
    if (!e || depth > 64) return -3;
    e = e->IgnoreParens();
    if (!tracked(e->getType().getUnqualifiedType()) &&
        !e->getType()->isRecordType() && !e->getType()->isPointerType()) return -3;
    if (const auto *member = dyn_cast<MemberExpr>(e)) {
      const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
      if (!field || field->getType()->isReferenceType()) return -3;
      if (!member->isArrow()) return storage(member->getBase(), depth + 1);
      return pointerRoot(member->getBase(), depth + 1);
    }
    if (const auto *conditional = dyn_cast<ConditionalOperator>(e)) {
      // Only glvalues denote storage. Both arms must have the same origin;
      // distinct private locals are also safe for caller-memory effects.
      if (!conditional->isGLValue()) return -3;
      int left = storage(conditional->getTrueExpr(), depth + 1);
      int right = storage(conditional->getFalseExpr(), depth + 1);
      return left == right ? left : -3;
    }
    if (const auto *deref = dyn_cast<UnaryOperator>(e)) {
      if (deref->getOpcode() != UO_Deref) return -3;
      return pointerRoot(deref->getSubExpr(), depth + 1);
    }
    if (const auto *element = dyn_cast<ArraySubscriptExpr>(e)) return pointerRoot(element->getBase(), depth + 1);
    const auto *v = variable(e);
    if (!v || !v->hasLocalStorage()) return -3;
    // The pointer slot itself is private; its pointee is selected only by a
    // dereference/member expression. A reference-to-pointer is borrowed storage.
    if (v->getType()->isPointerType()) return -2;
    auto found = parameters.find(v);
    if (found != parameters.end() && out->borrowed_parameters(found->second)) return found->second;
    auto alias = referenceAliases.find(v);
    if (alias != referenceAliases.end()) return alias->second;
    return tracked(v->getType()) ? -2 : -3;
  }
  void effect(const Expr *e, unsigned flags) {
    int index = storage(e);
    if (e && e->getType().isVolatileQualified()) flags |= 8;
    if (index == -3) complete = false;
    if (index >= 0) out->set_parameter_effects(index, out->parameter_effects(index) | flags);
  }
  bool VisitStmt(Stmt *s) {
    if (++nodes > 4096) { complete = false; return false; }
    if (!isa<CompoundStmt, DeclStmt, ReturnStmt, IfStmt, ForStmt, WhileStmt,
             DoStmt, SwitchStmt, CaseStmt, DefaultStmt, BreakStmt, ContinueStmt,
             NullStmt, DeclRefExpr, IntegerLiteral, FloatingLiteral,
             CharacterLiteral, CXXBoolLiteralExpr, CXXNullPtrLiteralExpr, GNUNullExpr, ImplicitCastExpr,
             ParenExpr, ExplicitCastExpr, UnaryOperator, BinaryOperator, CallExpr,
             ConditionalOperator, InitListExpr, CXXScalarValueInitExpr,
             ImplicitValueInitExpr, MemberExpr, ArraySubscriptExpr, StringLiteral>(s)) { complete = false; return false; }
    if (const auto *e = dyn_cast<Expr>(s))
      if (e->getType().isNull()) complete = false;
    return true;
  }
  bool VisitMemberExpr(MemberExpr *e) {
    // Conservatively include observable volatile access even for a discarded
    // glvalue; it must never become a read-only call certificate.
    if (e->getType().isVolatileQualified()) effect(e, 8);
    return true;
  }
  bool VisitVarDecl(VarDecl *v) {
    if (exactFunctionPointerTarget(v)) return true;
    if (!v->hasLocalStorage() || v->hasAttr<CleanupAttr>()) { complete = false; return true; }
    if (v->getType()->isLValueReferenceType() && borrowedType(v->getType()) && v->hasInit()) {
      // A reference cannot be rebound. Only accept exact scalar storage,
      // never a temporary, cast, global, field, or unknown returned reference.
      int origin = storage(v->getInit());
      if (origin == -3) complete = false;
      else referenceAliases[v] = origin;
    } else if (v->getType()->isPointerType() && !v->getType().isVolatileQualified()) {
      if (!v->hasInit()) delayedPointers.insert(v);
      else {
        int root = pointerRoot(v->getInit());
        if (root < 0) complete = false;
        else pointerAliases[v] = root;
      }
    } else if (!tracked(v->getType())) complete = false;
    return true;
  }
  bool TraverseUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *e) {
    // Fixed-size sizeof/alignof operands are unevaluated, including calls and
    // increments. VLA types can evaluate bound expressions and stay unknown.
    if (++nodes > 4096 || (e->getKind() != UETT_SizeOf && e->getKind() != UETT_AlignOf) ||
        e->getTypeOfArgument()->isVariablyModifiedType()) complete = false;
    return true;
  }
  bool VisitDeclRefExpr(DeclRefExpr *e) {
    const auto *v = dyn_cast<VarDecl>(e->getDecl());
    if (!v || exactFunctionPointerTarget(v)) return true;
    if (!v->hasLocalStorage()) complete = false;
    if (v->getType()->isPointerType()) {
      const Expr *top = e;
      for (unsigned depth = 0; depth < 8; ++depth) {
        auto parents = ctx.getParents(*top);
        if (parents.size() != 1) break;
        if (const auto *cast = parents[0].get<ImplicitCastExpr>()) {
          if (cast->getCastKind() == CK_PointerToBoolean) return true;
          top = cast; continue;
        }
        if (const auto *paren = parents[0].get<ParenExpr>()) { top = paren; continue; }
        if (const auto *cast = parents[0].get<ExplicitCastExpr>()) {
          if (pointerCast(cast)) { top = cast; continue; }
        }
        if (const auto *offset = parents[0].get<BinaryOperator>()) {
          if (pointerOffset(offset) && offset->getLHS() == top) { top = offset; continue; }
        }
        if (const auto *init = parents[0].get<VarDecl>())
          if (pointerAliases.count(init) && init->getInit() == top) return true;
        if (const auto *element = parents[0].get<ArraySubscriptExpr>())
          if (element->getBase() == top && storage(element) >= 0) return true;
        if (const auto *comparison = parents[0].get<BinaryOperator>()) {
          if (comparison->getOpcode() == BO_EQ || comparison->getOpcode() == BO_NE) return true;
          const auto *target = variable(comparison->getLHS());
          auto assigned = aliasAssignments.find(target);
          if (assigned != aliasAssignments.end() && assigned->second == comparison &&
              (comparison->getLHS() == top || comparison->getRHS() == top)) return true;
        }
        if (const auto *member = parents[0].get<MemberExpr>())
          if (member->isArrow() && storage(member) >= 0) return true;
        if (const auto *unary = parents[0].get<UnaryOperator>())
          if (unary->getOpcode() == UO_Deref && storage(unary) >= 0) return true;
        break;
      }
      if (!forwardedPointers.count(e)) complete = false;
    }
    return true;
  }
  // Built-in numeric conversions change values, not the memory-effect
  // certificate. Traverse the operand normally so writes, escapes, volatile
  // reads and transitive calls still contribute their effects. This does not
  // prove that a narrowing conversion itself is defined or initialized.
  bool VisitExplicitCastExpr(ExplicitCastExpr *e) {
    if (pointerCast(e)) return true;
    if (!tracked(e->getType()) || !tracked(e->getSubExpr()->getType())) {
      complete = false;
      return true;
    }
    switch (e->getCastKind()) {
      case CK_NoOp:
      case CK_IntegralCast:
      case CK_IntegralToBoolean:
      case CK_IntegralToFloating:
      case CK_FloatingToIntegral:
      case CK_FloatingToBoolean:
      case CK_FloatingCast:
        break;
      default:
        complete = false;
    }
    return true;
  }
  bool VisitImplicitCastExpr(ImplicitCastExpr *e) {
    if (e->getCastKind() == CK_LValueToRValue) {
      const auto *v = variable(e->getSubExpr());
      if (!e->getType()->isPointerType() || !v || !v->getType()->isPointerType())
        effect(e->getSubExpr(), 1);
    }
    if (e->getCastKind() == CK_PointerToIntegral || e->getCastKind() == CK_IntegralToPointer || (e->getCastKind() == CK_BitCast && !pointerCast(e)) || e->getCastKind() == CK_LValueBitCast) complete = false;
    return true;
  }
  bool VisitUnaryOperator(UnaryOperator *e) {
    if (e->isIncrementDecrementOp()) effect(e->getSubExpr(), 3);
    if (e->getOpcode() == UO_AddrOf && !borrowedAddresses.count(e)) complete = false;
    if (e->getOpcode() == UO_Deref && storage(e) < 0) complete = false;
    return true;
  }
  bool VisitBinaryOperator(BinaryOperator *e) {
    if (e->isAssignmentOp()) effect(e->getLHS(), e->isCompoundAssignmentOp() ? 3 : 2);
    if (e->getOpcode() == BO_Assign && bindDelayedPointer(e)) return true;
    if (e->getOpcode() == BO_Assign && e->getLHS()->getType()->isPointerType() && storage(e->getLHS()) >= 0) {
      const auto *rhs = e->getRHS()->IgnoreParenImpCasts();
      if (rhs->isNullPointerConstant(ctx, Expr::NPC_NeverValueDependent)) return true;
      if (const auto *ref = dyn_cast<DeclRefExpr>(rhs)) {
        if (isa<FunctionDecl>(ref->getDecl())) return true;
        const auto *v = dyn_cast<ParmVarDecl>(ref->getDecl());
        auto found = parameters.find(v);
        if (found != parameters.end() && v->getType()->isPointerType() && borrowedType(v->getType())) {
          out->set_parameter_effects(found->second, out->parameter_effects(found->second) | 4);
          forwardedPointers.insert(ref);
          return true;
        }
      }
      const Expr *retained = rhs;
      if (const auto *address = dyn_cast<UnaryOperator>(rhs)) {
        if (address->getOpcode() != UO_AddrOf) { complete = false; return true; }
        retained = address->getSubExpr();
        borrowedAddresses.insert(address);
      } else if (!isa<MemberExpr>(rhs)) {
        complete = false;
        return true;
      }
      int origin = storage(retained);
      // Retaining a loaded field may retain storage reachable through that
      // formal. Conservatively mark the whole root; no exact alias is claimed.
      if (origin >= 0) out->set_parameter_effects(origin, out->parameter_effects(origin) | 4);
      else complete = false;
      return true;
    }
    if ((e->getLHS()->getType()->isPointerType() || e->getRHS()->getType()->isPointerType()) &&
        e->getOpcode() != BO_EQ && e->getOpcode() != BO_NE && !pointerOffset(e)) complete = false;
    return true;
  }
  bool VisitReturnStmt(ReturnStmt *s) {
    if (!out->returns_borrowed()) return true;
    // Record every possible origin. The shared summary requires all leaves
    // to resolve to the same formal, including origins returned by callees.
    // Normal AST traversal still visits conditions and both branches for effects.
    std::vector<const Expr *> pending{s->getRetValue()};
    unsigned visited = 0;
    while (!pending.empty()) {
      if (++visited > 4096) { complete = false; break; }
      const auto *e = pending.back();
      pending.pop_back();
      e = e ? e->IgnoreParenImpCasts() : nullptr;
      if (const auto *conditional = dyn_cast_or_null<ConditionalOperator>(e)) {
        pending.push_back(conditional->getTrueExpr());
        pending.push_back(conditional->getFalseExpr());
        continue;
      }
      returns.push_back(e);
      if (const auto *ref = dyn_cast_or_null<DeclRefExpr>(e)) forwardedPointers.insert(ref);
      if (const auto *address = dyn_cast_or_null<UnaryOperator>(e))
        if (address->getOpcode() == UO_AddrOf) borrowedAddresses.insert(address);
    }
    return true;
  }
  bool VisitCallExpr(CallExpr *call) {
    const auto *callee = exactDirectCallee(call);
    if (!callee || isa<CXXMethodDecl>(callee) || callee->isVariadic() ||
        call->getNumArgs() != callee->getNumParams() ||
        (!tracked(call->getType()) && !call->getType()->isVoidType() && !call->getType()->isPointerType() && !scalarBorrowedType(call->getType()))) { complete = false; return true; }
    callIndices[call] = out->calls_size();
    auto *edge = out->add_calls();
    computeFuncEntityId(const_cast<FunctionDecl *>(callee), ctx, ctx.getSourceManager(), edge->mutable_callee());
    for (unsigned i = 0; i < callee->getNumParams(); ++i) {
      auto type = callee->getParamDecl(i)->getType();
      const auto *arg = call->getArg(i)->IgnoreParenImpCasts();
      int binding = -3;
      if (tracked(type)) binding = -1;
      else if (borrowedType(type)) {
        if (type->isLValueReferenceType()) binding = storage(arg);
        else if (isa<StringLiteral>(arg)) binding = -4;
        else if (const auto *address = dyn_cast<UnaryOperator>(arg)) {
          if (address->getOpcode() == UO_AddrOf) {
            borrowedAddresses.insert(address);
            binding = storage(address->getSubExpr());
          }
        } else if (const auto *ref = dyn_cast<DeclRefExpr>(arg)) {
          const auto *p = dyn_cast<ParmVarDecl>(ref->getDecl());
          auto found = parameters.find(p);
          if (found != parameters.end() && p->getType()->isPointerType() && borrowedType(p->getType())) {
            forwardedPointers.insert(ref);
            binding = found->second;
          }
        }
      }
      if (binding == -3) complete = false;
      edge->add_bindings(binding);
    }
    return true;
  }
 private:
  ASTContext &ctx;
  facts_pb::BorrowedEffects *out;
  bool complete = true;
  unsigned nodes = 0;
  std::map<const VarDecl *, unsigned> parameters;
  std::map<const VarDecl *, int> referenceAliases;
  std::map<const VarDecl *, int> pointerAliases;
  std::set<const VarDecl *> delayedPointers;
  std::map<const VarDecl *, const BinaryOperator *> aliasAssignments;
  std::map<const CallExpr *, unsigned> callIndices;
  std::vector<const Expr *> returns;
  std::set<const Expr *> forwardedPointers;
  std::set<const UnaryOperator *> borrowedAddresses;
};

const CXXRecordDecl *supportedRecord(QualType type) {
  if (type.isNull() || type.isVolatileQualified())
    return nullptr;
  const auto *record = type->getAsCXXRecordDecl();
  if (!record || !(record = record->getDefinition()) || record->isUnion() ||
      record->isDependentContext() || record->getNumBases() != 0 ||
      !record->isTriviallyCopyable() || !record->hasTrivialDestructor())
    return nullptr;
  unsigned count = 0;
  for (const auto *field : record->fields()) {
    if (!initializationScalar(field->getType()) || field->getType().isVolatileQualified() || field->isBitField() ||
        !field->getIdentifier())
      return nullptr;
    ++count;
  }
  return count > 0 && count <= 64 ? record : nullptr;
}

std::string recordIdentity(const CXXRecordDecl *record, ASTContext &ctx) {
  uint64_t lo = 0, hi = 0;
  odrDefinitionFingerprint(record, ctx, &lo, &hi);
  // The class identity alone is insufficient: equal field ordinals in two
  // inconsistent definitions may refer to different names or scalar types.
  std::string identity = mangleType(ctx.getRecordType(record), ctx) + ":" +
                         std::to_string(hi) + ":" + std::to_string(lo);
  const auto &layout = ctx.getASTRecordLayout(record);
  identity += ":size=" + std::to_string(layout.getSize().getQuantity()) +
              ":align=" + std::to_string(layout.getAlignment().getQuantity());
  for (const auto *field : record->fields()) {
    identity += "|" + std::to_string(field->getFieldIndex()) + ":" +
                field->getNameAsString() + ":" +
                mangleType(field->getType(), ctx) + ":offset=" +
                std::to_string(layout.getFieldOffset(field->getFieldIndex())) +
                ":bits=" + std::to_string(ctx.getTypeSize(field->getType())) +
                ":align=" + std::to_string(ctx.getTypeAlign(field->getType()));
  }
  return identity;
}

const CXXConstructExpr *directRecordConstruction(const Expr *expr) {
  if (!expr)
    return nullptr;
  expr = expr->IgnoreParenImpCasts();
  if (const auto *cleanup = dyn_cast<ExprWithCleanups>(expr))
    expr = cleanup->getSubExpr()->IgnoreParenImpCasts();
  return dyn_cast<CXXConstructExpr>(expr);
}

const ParmVarDecl *recordReceiverParameter(const FunctionDecl *function,
                                           unsigned &index) {
  if (!function || isa<CXXMethodDecl>(function) ||
      function->isOverloadedOperator() || function->isVariadic() ||
      !tracked(function->getReturnType()))
    return nullptr;
  const ParmVarDecl *receiver = nullptr;
  for (unsigned i = 0; i < function->getNumParams(); ++i) {
    const auto *parameter = function->getParamDecl(i);
    const auto type = parameter->getType();
    if (tracked(type))
      continue;
    if (receiver || !type->isLValueReferenceType() ||
        !type->getPointeeType().isConstQualified() ||
        type->getPointeeType().isVolatileQualified() ||
        !supportedRecord(type->getPointeeType()))
      return nullptr;
    receiver = parameter;
    index = i;
  }
  return receiver;
}

// Only immutable local aliases with fully enumerated scalar reads / direct
// scalar-borrow calls qualify. Unknown uses invalidate the entire alias.
class LocalAliases : public RecursiveASTVisitor<LocalAliases> {
 public:
  explicit LocalAliases(ASTContext &ctx) : ctx(ctx) {}
  bool VisitStmt(Stmt *) { if (++nodes > 8192) { exhausted = true; return false; } return true; }
  bool VisitVarDecl(VarDecl *decl) {
    if (checking || !decl->hasLocalStorage() || isa<ParmVarDecl>(decl) || !decl->hasInit() || decl->hasAttrs()) return true;
    const auto type = decl->getType();
    if (type.isVolatileQualified()) return true;
    if ((!type->isPointerType() && !type->isLValueReferenceType()) || !tracked(type->getPointeeType())) return true;
    const Expr *init = decl->getInit()->IgnoreParenImpCasts();
    if (type->isPointerType()) {
      const auto *address = dyn_cast<UnaryOperator>(init);
      if (!address || address->getOpcode() != UO_AddrOf) return true;
      init = address->getSubExpr()->IgnoreParenImpCasts();
    }
    const auto *ref = dyn_cast<DeclRefExpr>(init);
    const auto *target = ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
    if (target && target->hasLocalStorage() && tracked(target->getType()) &&
        ctx.hasSameUnqualifiedType(type->getPointeeType(), target->getType())) aliases[decl] = target;
    return true;
  }
  bool VisitDeclRefExpr(DeclRefExpr *ref) {
    if (!checking) return true;
    const auto *decl = dyn_cast<VarDecl>(ref->getDecl());
    if (!aliases.count(decl)) return true;
    const Expr *expr = ref;
    for (unsigned depth = 0; depth < 32; ++depth) {
      auto parents = ctx.getParents(*expr);
      if (parents.size() != 1) break;
      if (const auto *cast = parents[0].get<ImplicitCastExpr>()) {
        if (cast->getCastKind() == CK_LValueToRValue && tracked(cast->getType())) return true;
        if (cast->getCastKind() != CK_LValueToRValue && cast->getCastKind() != CK_NoOp) break;
        expr = cast; continue;
      }
      if (const auto *paren = parents[0].get<ParenExpr>()) { expr = paren; continue; }
      if (const auto *deref = parents[0].get<UnaryOperator>()) {
        if (decl->getType()->isPointerType() && deref->getOpcode() == UO_Deref) { expr = deref; continue; }
        break;
      }
      if (const auto *call = parents[0].get<CallExpr>()) {
        const auto *callee = exactDirectCallee(call);
        if (!callee || isa<CXXMethodDecl>(callee) || callee->isVariadic() || call->getNumArgs() != callee->getNumParams()) break;
        unsigned borrowed = 0; bool allConst = true, valid = true, actual = false;
        for (unsigned i = 0; i < callee->getNumParams(); ++i) {
          auto type = callee->getParamDecl(i)->getType();
          if (type->isPointerType() || type->isLValueReferenceType()) {
            ++borrowed;
            allConst = allConst && type->getPointeeType().isConstQualified();
            valid = valid && tracked(type->getPointeeType());
            if (call->getArg(i) == expr) actual = true;
          } else valid = valid && tracked(type);
        }
        if (valid && actual && (allConst || borrowed == 1)) return true;
      }
      break;
    }
    invalid.insert(decl);
    return true;
  }
  void collect(Stmt *body) {
    TraverseStmt(body); checking = true; TraverseStmt(body);
    for (const auto *decl : invalid) aliases.erase(decl);
    if (exhausted || aliases.size() > 512) aliases.clear();
  }
  std::map<const VarDecl *, const VarDecl *> aliases;
 private:
  ASTContext &ctx;
  bool checking = false;
  unsigned nodes = 0;
  bool exhausted = false;
  std::set<const VarDecl *> invalid;
};

// A disjoint record-output domain: one mutable plain record and scalar inputs.
// Unlike a constructor, this function receives an existing caller-owned object.
const ParmVarDecl *recordOutputParameter(const FunctionDecl *fn, unsigned &index) {
  if (!fn || isa<CXXMethodDecl>(fn) || fn->isVariadic() || !fn->getReturnType()->isVoidType()) return nullptr;
  const ParmVarDecl *result = nullptr;
  for (unsigned i = 0; i < fn->getNumParams(); ++i) {
    const auto *parameter = fn->getParamDecl(i);
    const auto type = parameter->getType();
    if (tracked(type)) continue;
    if (result || type.isVolatileQualified() || (!type->isPointerType() && !type->isLValueReferenceType()) ||
        type->getPointeeType().isConstQualified() || !supportedRecord(type->getPointeeType())) return nullptr;
    result = parameter;
    index = i;
  }
  return result;
}

class Flow : public RecursiveASTVisitor<Flow> {
public:
  bool shouldVisitImplicitCode() const { return true; }
  Flow(FunctionDecl *function, ASTContext &ctx)
      : function(function), ctx(ctx)
  {
    LocalAliases collected(ctx);
    collected.collect(function->getBody());
    aliases = std::move(collected.aliases);
    for (const auto &alias : aliases) {
      const auto *init = alias.first->getInit()->IgnoreParenImpCasts();
      if (const auto *address = dyn_cast<UnaryOperator>(init))
        if (address->getOpcode() == UO_AddrOf) borrowedAddresses.insert(address);
      const Expr *expr = alias.first->getInit();
      while (const auto *cast = dyn_cast<ImplicitCastExpr>(expr)) {
        if (cast->getCastKind() != CK_NoOp) break;
        borrowedCasts.insert(cast); expr = cast->getSubExpr();
      }
    }
    recordOutput = recordOutputParameter(function, recordOutputIndex);
    if (recordOutput) constructorRecord = supportedRecord(recordOutput->getType()->getPointeeType());
    receiverParameter =
        recordReceiverParameter(function, receiverParameterIndex);
    if (receiverParameter)
      receiverRecord =
          supportedRecord(receiverParameter->getType()->getPointeeType());
    unsigned count = 0;
    bool supported = true;
    for (unsigned i = 0; i < function->getNumParams(); ++i) {
      auto *parameter = function->getParamDecl(i);
      if (parameter == receiverParameter || parameter == recordOutput)
        continue;
      QualType type = parameter->getType();
      if (type->isReferenceType() || type->isPointerType()) {
        ++count;
        if (!tracked(type->getPointeeType()))
          supported = false;
        output = parameter;
        outputIndex = i;
      } else if (!tracked(type))
        supported = false;
    }
    if (supported && count > 0 && !isa<CXXMethodDecl>(function)) {
      bool allConst = true;
      for (const auto *parameter : function->parameters()) {
        const auto type = parameter->getType();
        if ((type->isPointerType() || type->isReferenceType()) &&
            !type->getPointeeType().isConstQualified()) allConst = false;
      }
      if (allConst) {
        for (unsigned i = 0; i < function->getNumParams(); ++i) {
          const auto *parameter = function->getParamDecl(i);
          if (parameter->getType()->isPointerType() || parameter->getType()->isReferenceType()) readonlyParameters[parameter] = i;
        }
        output = nullptr;
        count = 0;
      }
    }
    if (const auto *constructor = dyn_cast<CXXConstructorDecl>(function)) {
      if (supported && count == 0 && !constructor->isDelegatingConstructor())
        constructorRecord =
            supportedRecord(ctx.getRecordType(constructor->getParent()));
    }
    if (const auto *method = dyn_cast<CXXMethodDecl>(function))
      if (!constructorRecord && supported && count == 0 && method->isConst() &&
          !method->isVolatile() && !method->isVirtual() &&
          !method->isStatic() && !method->isVariadic() &&
          tracked(method->getReturnType()))
        receiverRecord =
            supportedRecord(ctx.getRecordType(method->getParent()));
    if (!supported || count > 1 || function->isVariadic() ||
        (isa<CXXMethodDecl>(function) && !constructorRecord &&
         !receiverRecord)) {
      output = nullptr;
      ownIsolated = false;
    }
    if (!function->getReturnType()->isVoidType() &&
        !tracked(function->getReturnType()))
      ownIsolated = false;
  }
  struct Object {
    const CXXRecordDecl *record = nullptr;
    std::vector<uint32_t> fields;
  };
  const FieldDecl *constructorField(const Expr *expr) const {
    if (!constructorRecord || !expr)
      return nullptr;
    const auto *member = dyn_cast<MemberExpr>(expr->IgnoreParenImpCasts());
    if (!member) return nullptr;
    if (recordOutput) {
      const auto *ref = dyn_cast<DeclRefExpr>(member->getBase()->IgnoreParenImpCasts());
      if (!ref || ref->getDecl() != recordOutput || member->isArrow() != recordOutput->getType()->isPointerType() || reboundBorrowedPointers.count(recordOutput)) return nullptr;
    } else if (!isa<CXXThisExpr>(member->getBase()->IgnoreParenImpCasts())) return nullptr;
    const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
    return field && field->getParent()->getCanonicalDecl() ==
                        constructorRecord->getCanonicalDecl()
               ? field
               : nullptr;
  }
  const FieldDecl *receiverField(const Expr *expr) const {
    if (!receiverRecord || !expr)
      return nullptr;
    const auto *member = dyn_cast<MemberExpr>(expr->IgnoreParenImpCasts());
    if (!member)
      return nullptr;
    const auto *base = member->getBase()->IgnoreParenImpCasts();
    if (receiverParameter) {
      const auto *reference = dyn_cast<DeclRefExpr>(base);
      if (member->isArrow() || !reference ||
          reference->getDecl() != receiverParameter)
        return nullptr;
    } else if (!isa<CXXThisExpr>(base))
      return nullptr;
    const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
    return field && field->getParent()->getCanonicalDecl() ==
                        receiverRecord->getCanonicalDecl()
               ? field
               : nullptr;
  }
  const VarDecl *recognizeGetter(const CallExpr *call) const {
    const auto *memberCall = dyn_cast<CXXMemberCallExpr>(call);
    const auto *method = memberCall ? memberCall->getMethodDecl() : nullptr;
    if (!method || !method->isConst() || method->isVolatile() ||
        method->isVirtual() || method->isStatic() || method->isVariadic() ||
        !tracked(method->getReturnType()) ||
        call->getNumArgs() != method->getNumParams())
      return nullptr;
    for (const auto *parameter : method->parameters())
      if (!tracked(parameter->getType()))
        return nullptr;
    const auto *ref = dyn_cast<DeclRefExpr>(
        memberCall->getImplicitObjectArgument()->IgnoreParenImpCasts());
    const auto *variable = ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
    auto found = objects.find(variable);
    if (found == objects.end() || method->getParent()->getCanonicalDecl() !=
                                      found->second.record->getCanonicalDecl())
      return nullptr;
    return variable;
  }
  const VarDecl *recognizeRecordArgument(const CallExpr *call,
                                         unsigned &index) const {
    const auto *callee = exactDirectCallee(call);
    const auto *parameter = recordReceiverParameter(callee, index);
    if (!parameter || call->getNumArgs() != callee->getNumParams())
      return nullptr;
    const auto *reference =
        dyn_cast<DeclRefExpr>(call->getArg(index)->IgnoreParenImpCasts());
    const auto *variable =
        reference ? dyn_cast<VarDecl>(reference->getDecl()) : nullptr;
    auto found = objects.find(variable);
    const auto *record =
        supportedRecord(parameter->getType()->getPointeeType());
    if (found == objects.end() ||
        found->second.record->getCanonicalDecl() != record->getCanonicalDecl())
      return nullptr;
    return variable;
  }
  const VarDecl *objectBase(const Expr *expr) const {
    if (!expr)
      return nullptr;
    const auto *member = dyn_cast<MemberExpr>(expr->IgnoreParenImpCasts());
    if (!member || member->isArrow())
      return nullptr;
    const auto *ref =
        dyn_cast<DeclRefExpr>(member->getBase()->IgnoreParenImpCasts());
    const auto *variable = ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
    auto found = objects.find(variable);
    const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
    return found != objects.end() && field &&
                   field->getParent()->getCanonicalDecl() ==
                       found->second.record->getCanonicalDecl()
               ? variable
               : nullptr;
  }
  bool objectFieldSlot(const Expr *expr, uint32_t &slot) const {
    const auto *variable = objectBase(expr);
    if (!variable)
      return false;
    const auto *member = cast<MemberExpr>(expr->IgnoreParenImpCasts());
    const auto *field = cast<FieldDecl>(member->getMemberDecl());
    const auto &fields = objects.find(variable)->second.fields;
    if (field->getFieldIndex() >= fields.size())
      return false;
    slot = fields[field->getFieldIndex()];
    return true;
  }
  void poisonObject(const VarDecl *variable) {
    if (objects.count(variable))
      escapedObjects.insert(variable);
  }
  const VarDecl *aliasStorage(const Expr *expr) const {
    if (!expr) return nullptr;
    expr = expr->IgnoreParenImpCasts();
    bool pointer = false;
    if (const auto *deref = dyn_cast<UnaryOperator>(expr)) {
      if (deref->getOpcode() != UO_Deref) return nullptr;
      expr = deref->getSubExpr()->IgnoreParenImpCasts(); pointer = true;
    }
    const auto *ref = dyn_cast<DeclRefExpr>(expr);
    const auto *decl = ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
    auto found = aliases.find(decl);
    return found != aliases.end() && decl->getType()->isPointerType() == pointer ? found->second : nullptr;
  }
  struct BorrowedCall {
    unsigned argument = 0;
    const VarDecl *local = nullptr;
    bool parameter = false;
    bool readonlyParameter = false;
    unsigned parameterIndex = 0;
  };
  int readonlyStorage(const Expr *expr) const {
    if (!expr) return -1;
    expr = expr->IgnoreParenImpCasts();
    bool pointer = false;
    if (const auto *deref = dyn_cast<UnaryOperator>(expr)) {
      if (deref->getOpcode() != UO_Deref) return -1;
      expr = deref->getSubExpr()->IgnoreParenImpCasts();
      pointer = true;
    }
    const auto *ref = dyn_cast<DeclRefExpr>(expr);
    const auto *parameter = ref ? dyn_cast<ParmVarDecl>(ref->getDecl()) : nullptr;
    auto found = readonlyParameters.find(parameter);
    return found != readonlyParameters.end() && !reboundBorrowedPointers.count(parameter) &&
                   !escaped.count(parameter) && parameter->getType()->isPointerType() == pointer
               ? int(found->second) : -1;
  }
  bool outputStorage(const Expr *expr) const {
    if (!output || !expr || reboundBorrowedPointers.count(output) || escaped.count(output))
      return false;
    expr = expr->IgnoreParenImpCasts();
    if (output->getType()->isReferenceType()) {
      const auto *ref = dyn_cast<DeclRefExpr>(expr);
      return ref && ref->getDecl() == output;
    }
    const auto *deref = dyn_cast<UnaryOperator>(expr);
    if (!deref || deref->getOpcode() != UO_Deref)
      return false;
    const auto *ref =
        dyn_cast<DeclRefExpr>(deref->getSubExpr()->IgnoreParenImpCasts());
    return ref && ref->getDecl() == output;
  }
  bool recognizeBorrow(CallExpr *call, const FunctionDecl *callee,
                       BorrowedCall &binding) {
    if (isa<CXXMethodDecl>(callee) ||
        callee->isVariadic() || call->getNumArgs() != callee->getNumParams())
      return false;
    unsigned count = 0;
    for (unsigned i = 0; i < callee->getNumParams(); ++i) {
      QualType type = callee->getParamDecl(i)->getType();
      if (type->isReferenceType() || type->isPointerType()) {
        if (!tracked(type->getPointeeType()))
          return false;
        binding.argument = i;
        ++count;
      } else if (!tracked(type))
        return false;
    }
    if (count != 1)
      return false;
    if (!standalone(call) && !callee->getParamDecl(binding.argument)->getType()->getPointeeType().isConstQualified())
      return false;
    return bindBorrow(call, callee, binding);
  }
  bool recognizeReadonly(CallExpr *call, const FunctionDecl *callee, std::vector<BorrowedCall> &bindings) {
    if (isa<CXXMethodDecl>(callee) || callee->isVariadic() || call->getNumArgs() != callee->getNumParams()) return false;
    for (unsigned i = 0; i < callee->getNumParams(); ++i) {
      const auto type = callee->getParamDecl(i)->getType();
      if (tracked(type)) continue;
      if ((!type->isPointerType() && !type->isLValueReferenceType()) ||
          !tracked(type->getPointeeType()) || !type->getPointeeType().isConstQualified()) return false;
      BorrowedCall binding;
      binding.argument = i;
      if (!bindBorrow(call, callee, binding)) return false;
      bindings.push_back(binding);
    }
    return !bindings.empty();
  }
  bool bindBorrow(CallExpr *call, const FunctionDecl *callee, BorrowedCall &binding) {
    const Expr *actual = call->getArg(binding.argument)->IgnoreParenImpCasts();
    QualType parameterType = callee->getParamDecl(binding.argument)->getType();
    const UnaryOperator *address = nullptr;
    if (parameterType->isPointerType()) {
      const auto *alias = dyn_cast<DeclRefExpr>(actual);
      const auto *decl = alias ? dyn_cast<VarDecl>(alias->getDecl()) : nullptr;
      if (decl && decl->getType()->isPointerType() && aliases.count(decl)) actual = decl->getInit()->IgnoreParenImpCasts();
      const auto *reference = dyn_cast<DeclRefExpr>(actual);
      const auto *parameter = reference ? dyn_cast<ParmVarDecl>(reference->getDecl()) : nullptr;
      auto input = readonlyParameters.find(parameter);
      if (input != readonlyParameters.end() && parameter->getType()->isPointerType() &&
          ctx.hasSameUnqualifiedType(parameterType->getPointeeType(), parameter->getType()->getPointeeType())) {
        binding.readonlyParameter = true;
        binding.parameterIndex = input->second;
        return true;
      }
      if (output && output->getType()->isPointerType()) {
        const auto *ref = dyn_cast<DeclRefExpr>(actual);
        if (ref && ref->getDecl() == output) {
          binding.parameter = true;
          return ctx.hasSameUnqualifiedType(
              parameterType->getPointeeType(),
              output->getType()->getPointeeType());
        }
      }
      address = dyn_cast<UnaryOperator>(actual);
      if (!address || address->getOpcode() != UO_AddrOf)
        return false;
      actual = address->getSubExpr()->IgnoreParenImpCasts();
    }
    if (!ctx.hasSameUnqualifiedType(parameterType->getPointeeType(),
                                    actual->getType()))
      return false;
    const int input = readonlyStorage(actual);
    if (input >= 0) {
      binding.readonlyParameter = true;
      binding.parameterIndex = input;
    } else if (outputStorage(actual))
      binding.parameter = true;
    else {
      const auto *ref = dyn_cast<DeclRefExpr>(actual);
      const auto *variable = ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
      if (const auto *alias = aliasStorage(actual)) variable = alias;
      if (!variable || !variable->hasLocalStorage() ||
          variable->getType()->isReferenceType() ||
          !initializationScalar(variable->getType()))
        return false;
      binding.local = variable;
    }
    if (address)
      borrowedAddresses.insert(address);
    if (parameterType->isReferenceType()) {
      const Expr *argument = call->getArg(binding.argument);
      while (const auto *cast = dyn_cast<ImplicitCastExpr>(argument)) {
        if (!cast->isGLValue() || cast->getCastKind() != CK_NoOp) break;
        borrowedCasts.insert(cast);
        argument = cast->getSubExpr();
      }
    }
    return true;
  }
  bool VisitVarDecl(VarDecl *decl) {
    // Cleanup callbacks are implicit calls at scope exit, absent from the
    // ordinary CallExpr traversal and this IR's ordered operations.
    if (decl->hasAttr<CleanupAttr>()) {
      complete = false;
      ownIsolated = false;
    }
    if (!decl->hasLocalStorage())
      ownIsolated = false;
    const auto *record = decl->hasLocalStorage()
                             ? supportedRecord(decl->getType())
                             : nullptr;
    const auto *construction =
        record ? directRecordConstruction(decl->getInit()) : nullptr;
    bool supportedConstruction =
        construction &&
        construction->getConstructionKind() == CXXConstructionKind::Complete &&
        !construction->getConstructor()->isCopyOrMoveConstructor() &&
        construction->getNumArgs() ==
            construction->getConstructor()->getNumParams();
    if (supportedConstruction) {
      for (const auto *parameter : construction->getConstructor()->parameters())
        if (!tracked(parameter->getType()))
          supportedConstruction = false;
    }
    if (construction && construction->getConstructionKind() == CXXConstructionKind::Complete &&
        construction->getConstructor()->isCopyConstructor() &&
        construction->getConstructor()->isTrivial() && construction->getNumArgs() == 1) {
      const auto *ref = dyn_cast<DeclRefExpr>(construction->getArg(0)->IgnoreParenImpCasts());
      const auto *source = ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
      auto found = objects.find(source);
      if (found != objects.end() && found->second.record == record) {
        supportedConstruction = true;
        copies.emplace(construction, source);
        reads.emplace(construction, reads.size() + 1);
      }
    }
    const bool valueRecord = record && decl->hasInit() &&
        !isa<ParmVarDecl>(decl) && !decl->getType()->isReferenceType() &&
        !construction;
    if ((supportedConstruction || valueRecord || (record && isa<ParmVarDecl>(decl))) && !objects.count(decl)) {
      Object object;
      object.record = record;
      for (const auto *field : record->fields()) {
        (void)field;
        object.fields.push_back(localCount++);
      }
      objects.emplace(decl, std::move(object));
      if (construction) constructions.emplace(construction, decl);
    }
    if (!aliases.count(decl) && decl != output && decl != recordOutput && !readonlyParameters.count(dyn_cast<ParmVarDecl>(decl)) && decl != receiverParameter &&
        !tracked(decl->getType()) && !supportedConstruction)
      ownIsolated = false;
    if (decl->getType()->isReferenceType() && decl->hasInit() && !aliases.count(decl))
      expose(decl->getInit());
    // A pointer formal has two distinct states: its own value (IV_PARAMETER)
    // and the storage it designates (IV_OUTPUT / IV_BORROWED_INPUT). Track the
    // pointer slot even when the pointee participates in the borrowed model.
    // References have no independent value slot and remain excluded.
    if (decl != recordOutput && decl->hasLocalStorage() && initializationScalar(decl->getType()) &&
        !locals.count(decl))
      locals.emplace(decl, localCount++);
    return true;
  }
  bool VisitDeclRefExpr(DeclRefExpr *expr) {
    if (const auto *decl = dyn_cast<VarDecl>(expr->getDecl()))
      if (!decl->hasLocalStorage())
        ownIsolated = false;
    return true;
  }
  bool VisitMemberExpr(MemberExpr *expr) {
    uint32_t slot = 0;
    bool getterName = false;
    auto parents = ctx.getParents(*expr);
    if (parents.size() == 1)
      if (const auto *call = parents[0].get<CXXMemberCallExpr>())
        getterName = getterCalls.count(call) != 0;
    if (!constructorField(expr) && !receiverField(expr) &&
        !objectFieldSlot(expr, slot) && !getterName)
      ownIsolated = false;
    return true;
  }
  bool VisitCXXThisExpr(CXXThisExpr *expr) {
    auto parents = ctx.getParents(*expr);
    const auto *member =
        parents.size() == 1 ? parents[0].get<MemberExpr>() : nullptr;
    if (!member || (!constructorField(member) && !receiverField(member)))
      ownIsolated = false;
    return true;
  }
  bool VisitArraySubscriptExpr(ArraySubscriptExpr *) {
    ownIsolated = false;
    return true;
  }
  bool VisitStmt(Stmt *stmt) {
    // Exception allocation/copy and exception-object retention are not
    // represented by the isolated scalar effect summary.
    if (isa<CXXThrowExpr>(stmt))
      ownIsolated = false;
    if (++nodes > 8192) {
      complete = false;
      return false;
    }
    // Direct goto edges are represented by the CFG, including backward edges
    // and joins that skip assignments. Computed jumps remain unsupported.
    if (isa<AsmStmt, IndirectGotoStmt, CXXTryStmt, SEHTryStmt,
            LambdaExpr, CoroutineBodyStmt, StmtExpr>(stmt))
      complete = false;
    return true;
  }
  // Escapes permanently poison just their scalar storage. A later assignment
  // cannot restore a proof because a retained alias may write again at any
  // call.
  void exposePointee(const Expr *expr) {
    if (!expr) { poisonAll = true; return; }
    expr = expr->IgnoreParenImpCasts();
    if (const auto *address = dyn_cast<UnaryOperator>(expr)) {
      if (address->getOpcode() == UO_AddrOf) expose(address->getSubExpr());
    }
    if (const auto *ref = dyn_cast<DeclRefExpr>(expr)) {
      const auto *decl = dyn_cast<VarDecl>(ref->getDecl());
      auto alias = aliases.find(decl);
      if (alias != aliases.end()) escaped.insert(alias->second);
    }
    // Other pointees can only alias private storage after an address escape,
    // which the whole-function traversal already poisons independently.
  }
  void expose(const Expr *expr, unsigned depth = 0) {
    ownIsolated = false;
    if (!expr || depth > 64) {
      poisonAll = true;
      return;
    }
    expr = expr->IgnoreParens();
    if (const auto *cast = dyn_cast<CastExpr>(expr)) {
      expose(cast->getSubExpr(), depth + 1);
    } else if (const auto *ref = dyn_cast<DeclRefExpr>(expr)) {
      const auto *decl = dyn_cast<VarDecl>(ref->getDecl());
      if (decl)
        poisonObject(decl);
      if (!decl) {
        poisonAll = true;
        return;
      }
      if (aliases.count(decl)) escaped.insert(aliases.find(decl)->second);
      if (decl->getType()->isReferenceType() && decl->hasInit())
        expose(decl->getInit(), depth + 1);
      else if (decl->hasLocalStorage() && initializationScalar(decl->getType()))
        escaped.insert(decl);
      // Named globals, non-scalar objects and incoming references are not
      // storage for a newly created local scalar. Any scalar stored in such
      // an object was poisoned at the earlier address/reference binding.
    } else if (const auto *variable = objectBase(expr)) {
      poisonObject(variable);
    } else if (constructorField(expr)) {
      // An escaped subobject can invalidate any later constructor guarantee.
      ownIsolated = false;
    } else if (const auto *member = dyn_cast<MemberExpr>(expr)) {
      // A write through an object/pointer cannot reach an unrelated local
      // whose address never escaped. Follow the base so tracked objects and
      // reference aliases still lose their guarantees.
      if (member->isArrow()) exposePointee(member->getBase());
      else expose(member->getBase(), depth + 1);
    } else if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(expr)) {
      exposePointee(subscript->getBase());
    } else if (const auto *unary = dyn_cast<UnaryOperator>(expr)) {
      if (unary->getOpcode() == UO_Deref)
        exposePointee(unary->getSubExpr());
      else
        poisonAll = true;
    } else if (const auto *conditional = dyn_cast<ConditionalOperator>(expr)) {
      expose(conditional->getTrueExpr(), depth + 1);
      expose(conditional->getFalseExpr(), depth + 1);
    } else if (const auto *binary = dyn_cast<BinaryOperator>(expr)) {
      if (binary->getOpcode() == BO_Comma)
        expose(binary->getRHS(), depth + 1);
      else
        poisonAll = true;
    } else if (isa<MaterializeTemporaryExpr, CXXBindTemporaryExpr>(expr)) {
      // A temporary's own scalar storage is distinct from every tracked local.
      // Construction arguments are inspected independently by the visitor.
    } else {
      poisonAll = true;
    }
  }
  bool standalone(const Expr *expr) {
    // Accept discarded full expressions by their statement position, not by
    // whether the author put braces around the body. Never accept conditions,
    // arguments, returns or nested assignments whose value is consumed.
    const Stmt *statement = expr;
    for (unsigned depth = 0; depth < 64; ++depth) {
      auto parents = ctx.getParents(*statement);
      if (parents.size() != 1) return false;
      if (const auto *paren = parents[0].get<ParenExpr>()) {
        statement = paren;
        continue;
      }
      // Clang wraps expressions containing materialized trivial temporaries
      // even when there is no destructor/cleanup effect to model. The raw
      // assignment remains an ordered CFG element inside this wrapper.
      if (const auto *cleanups = parents[0].get<ExprWithCleanups>()) {
        if (cleanups->cleanupsHaveSideEffects()) return false;
        statement = cleanups;
        continue;
      }
      if (parents[0].get<CompoundStmt>()) return true;
      if (const auto *branch = parents[0].get<IfStmt>())
        return branch->getThen() == statement || branch->getElse() == statement;
      if (const auto *loop = parents[0].get<ForStmt>())
        return loop->getInit() == statement || loop->getInc() == statement ||
               loop->getBody() == statement;
      if (const auto *loop = parents[0].get<WhileStmt>())
        return loop->getBody() == statement;
      if (const auto *loop = parents[0].get<DoStmt>())
        return loop->getBody() == statement;
      if (const auto *label = parents[0].get<SwitchCase>())
        return label->getSubStmt() == statement;
      if (const auto *label = parents[0].get<LabelStmt>())
        return label->getSubStmt() == statement;
      return false;
    }
    return false;
  }
  bool VisitUnaryOperator(UnaryOperator *expr) {
    if (expr->getOpcode() == UO_AddrOf && !borrowedAddresses.count(expr))
      expose(expr->getSubExpr());
    if (expr->getOpcode() == UO_Deref && !outputStorage(expr) && readonlyStorage(expr) < 0 && !aliasStorage(expr))
      ownIsolated = false;
    // The value of nested ++/-- would need an expression temporary. A
    // discarded standalone update only preserves its old initialization.
    if (expr->isIncrementDecrementOp()) {
      reads.emplace(expr, reads.size() + 1);
      if (!standalone(expr))
        complete = false;
      const auto *ref =
          dyn_cast<DeclRefExpr>(expr->getSubExpr()->IgnoreParenImpCasts());
      const auto *variable = ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
      invalidateBorrowedPointer(variable);
      uint32_t fieldSlot = 0;
      if (!outputStorage(expr->getSubExpr()) &&
          !constructorField(expr->getSubExpr()) &&
          !objectFieldSlot(expr->getSubExpr(), fieldSlot) &&
          (!variable || !variable->hasLocalStorage() ||
           !initializationScalar(variable->getType()))) {
        ownIsolated = false;
        expose(expr->getSubExpr());
      }
    }
    return true;
  }
  void invalidateBorrowedPointer(const VarDecl *variable) {
    if (variable && variable->getType()->isPointerType() &&
        (variable == output || variable == recordOutput || readonlyParameters.count(dyn_cast<ParmVarDecl>(variable)))) {
      // The borrowed-state slot names the original pointee. Rebinding or
      // arithmetic requires alias tracking before subsequent accesses can use it.
      reboundBorrowedPointers.insert(variable);
      ownIsolated = false;
    }
  }
  bool VisitBinaryOperator(BinaryOperator *expr) {
    if (!expr->isAssignmentOp())
      return true;
    if (expr->isCompoundAssignmentOp()) {
      reads.emplace(expr, reads.size() + 1);
      // Pointer arithmetic and overloaded operators need separate models.
      if (!initializationScalar(expr->getLHS()->getType()))
        complete = false;
    }
    const auto *leftRef =
        dyn_cast<DeclRefExpr>(expr->getLHS()->IgnoreParenImpCasts());
    const auto *leftVar =
        leftRef ? dyn_cast<VarDecl>(leftRef->getDecl()) : nullptr;
    invalidateBorrowedPointer(leftVar);
    uint32_t fieldSlot = 0;
    if (!outputStorage(expr->getLHS()) && !constructorField(expr->getLHS()) &&
        !objectFieldSlot(expr->getLHS(), fieldSlot) &&
        (!leftVar || !leftVar->hasLocalStorage() ||
         !initializationScalar(leftVar->getType()))) {
      ownIsolated = false;
      expose(expr->getLHS());
    }
    // Only standalone assignments are supported. In particular x=(x=...)
    // and assignments in call arguments must not reuse stale expression values.
    if (!standalone(expr))
      complete = false;
    return true;
  }
  bool VisitCallExpr(CallExpr *call) {
    const auto *callee = exactDirectCallee(call);
    if (const auto *assignment = dyn_cast<CXXOperatorCallExpr>(call)) {
      const auto *method = dyn_cast_or_null<CXXMethodDecl>(callee);
      if (method && method->isTrivial() &&
          (method->isCopyAssignmentOperator() || method->isMoveAssignmentOperator()) &&
          assignment->getNumArgs() == 2 && standalone(assignment)) {
        const auto *ref = dyn_cast<DeclRefExpr>(assignment->getArg(0)->IgnoreParenImpCasts());
        const auto *decl = ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
        auto object = objects.find(decl);
        if (object != objects.end() && method->getParent()->getCanonicalDecl() == object->second.record->getCanonicalDecl()) {
          recordAssignments.emplace(call, decl);
          return true;
        }
      }
    }
    emitMathContract(callee, ctx);
    if (!callee) {
      complete = false;
      ownIsolated = false;
    }
    const auto *getterObject = recognizeGetter(call);
    unsigned recordIndex = 0;
    if (!getterObject) {
      getterObject = recognizeRecordArgument(call, recordIndex);
      if (getterObject)
        recordArgumentCalls.emplace(call, recordIndex);
    }
    if (getterObject)
      getterCalls.emplace(call, getterObject);
    if (const auto *memberCall = dyn_cast<CXXMemberCallExpr>(call))
      if (!getterObject)
        expose(memberCall->getImplicitObjectArgument());
    if (const auto *operatorCall = dyn_cast<CXXOperatorCallExpr>(call))
      if (const auto *method = dyn_cast_or_null<CXXMethodDecl>(callee))
        if (!method->isStatic() && operatorCall->getNumArgs() > 0)
          expose(operatorCall->getArg(0));
    if (callee) {
      facts_pb::EntityId calleeId;
      computeFuncEntityId(const_cast<FunctionDecl *>(callee), ctx,
                          ctx.getSourceManager(), &calleeId);
      effectCallees[{calleeId.id_hi(), calleeId.id()}] = calleeId;
      unsigned outputIndex = 0;
      const auto *outputParameter = recordOutputParameter(callee, outputIndex);
      if (outputParameter && call->getNumArgs() == callee->getNumParams() && standalone(call)) {
        const Expr *actual = call->getArg(outputIndex)->IgnoreParenImpCasts();
        const UnaryOperator *address = nullptr;
        if (outputParameter->getType()->isPointerType()) {
          address = dyn_cast<UnaryOperator>(actual);
          actual = address && address->getOpcode() == UO_AddrOf ? address->getSubExpr()->IgnoreParenImpCasts() : nullptr;
        }
        const auto *ref = actual ? dyn_cast<DeclRefExpr>(actual) : nullptr;
        const auto *variable = ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
        auto object = objects.find(variable);
        if (object != objects.end() && object->second.record == supportedRecord(outputParameter->getType()->getPointeeType())) {
          recordOutputCalls.emplace(call, std::make_pair(variable, outputIndex));
          if (address) borrowedAddresses.insert(address);
          else {
            const Expr *argument = call->getArg(outputIndex);
            while (const auto *cast = dyn_cast<ImplicitCastExpr>(argument)) {
              if (!cast->isGLValue() || cast->getCastKind() != CK_NoOp) break;
              borrowedCasts.insert(cast); argument = cast->getSubExpr();
            }
          }
        }
      }
      BorrowedCall binding;
      auto oldAddresses = borrowedAddresses;
      auto oldCasts = borrowedCasts;
      std::vector<BorrowedCall> inputs;
      const bool readonly = recognizeReadonly(call, callee, inputs);
      if (readonly) readonlyCalls.emplace(call, inputs);
      else { borrowedAddresses = oldAddresses; borrowedCasts = oldCasts; }
      const bool borrowed = !readonly && recognizeBorrow(call, callee, binding);
      if (borrowed)
        borrowedCalls.emplace(call, binding);
      // A syntactically simple alias call may still be outside the ordered
      // transfer model (for example a mutable call nested in an expression).
      // Its initializer was exempted from escape poisoning, so restore that
      // conservative behavior explicitly whenever no binding was accepted.
      for (unsigned i = 0; i < call->getNumArgs(); ++i) {
        if ((borrowed && i == binding.argument) ||
            (readonly && i < callee->getNumParams() &&
             (callee->getParamDecl(i)->getType()->isPointerType() ||
              callee->getParamDecl(i)->getType()->isReferenceType()))) continue;
        const auto *reference = dyn_cast<DeclRefExpr>(call->getArg(i)->IgnoreParenImpCasts());
        const auto *variable = reference ? dyn_cast<VarDecl>(reference->getDecl()) : nullptr;
        if (aliases.count(variable) || aliasStorage(call->getArg(i))) expose(call->getArg(i));
      }
      if ((isa<CXXMethodDecl>(callee) && !getterObject) ||
          callee->isVariadic() ||
          (!tracked(call->getType()) && !call->getType()->isVoidType()))
        ownIsolated = false;
      for (unsigned i = 0; i < callee->getNumParams(); ++i)
        if (!tracked(callee->getParamDecl(i)->getType()) &&
            !(borrowed && i == binding.argument) &&
            !(readonly && (callee->getParamDecl(i)->getType()->isPointerType() || callee->getParamDecl(i)->getType()->isReferenceType())) &&
            !(recordArgumentCalls.count(call) && i == recordIndex) &&
            !(recordOutputCalls.count(call) && i == recordOutputCalls.find(call)->second.second))
          ownIsolated = false;
      const auto *identifier = callee->getIdentifier();
      const auto name = identifier ? identifier->getName() : llvm::StringRef();
      if (callee->hasAttr<ReturnsTwiceAttr>() || name == "setjmp" ||
          name == "_setjmp" || name == "sigsetjmp" || name == "__sigsetjmp" ||
          name == "__builtin_setjmp" || name == "longjmp" ||
          name == "_longjmp" || name == "siglongjmp" ||
          name == "__builtin_longjmp")
        complete = false;
      unsigned offset = 0;
      if (isa<CXXOperatorCallExpr>(call))
        if (const auto *method = dyn_cast<CXXMethodDecl>(callee))
          if (!method->isStatic())
            offset = 1;
      for (unsigned i = 0; i < callee->getNumParams(); ++i)
        if (callee->getParamDecl(i)->getType()->isReferenceType() &&
            !(borrowed && i == binding.argument) &&
            !(readonly && (callee->getParamDecl(i)->getType()->isPointerType() || callee->getParamDecl(i)->getType()->isReferenceType())) &&
            !(recordArgumentCalls.count(call) && i == recordIndex) &&
            !(recordOutputCalls.count(call) && i == recordOutputCalls.find(call)->second.second)) {
          if (i + offset < call->getNumArgs())
            expose(call->getArg(i + offset));
          else
            poisonAll = true;
        }
    }
    return true;
  }
  bool VisitInitListExpr(InitListExpr *expr) {
    if (!tracked(expr->getType()))
      ownIsolated = false;
    // Value initialization inserts an lvalue-to-rvalue cast. Remaining
    // glvalues may bind aggregate reference members, including nested lists.
    for (const auto *init : expr->inits())
      if (init && init->isGLValue())
        expose(init);
    return true;
  }
  bool VisitCXXParenListInitExpr(CXXParenListInitExpr *expr) {
    ownIsolated = false;
    for (const auto *init : expr->getInitExprs())
      if (init && init->isGLValue()) expose(init);
    return true;
  }
  bool VisitCXXDeleteExpr(CXXDeleteExpr *) {
    ownIsolated = false;
    return true;
  }
  bool VisitCastExpr(CastExpr *expr) {
    if (expr->getCastKind() == CK_PointerToIntegral ||
        expr->getCastKind() == CK_BitCast ||
        expr->getCastKind() == CK_LValueBitCast)
      ownIsolated = false;
    if (expr->getCastKind() == CK_IntegralToPointer) {
      poisonAll = true;
      ownIsolated = false;
    }
    if (expr->isGLValue() && initializationScalar(expr->getSubExpr()->getType()) && !borrowedCasts.count(expr))
      expose(expr->getSubExpr());
    return true;
  }
  bool VisitCXXConstructExpr(CXXConstructExpr *expr) {
    if (copies.count(expr)) return true;
    if (expr->getConstructor()->isTrivial() && expr->getConstructor()->isCopyOrMoveConstructor() &&
        supportedRecord(expr->getType()) && expr->getNumArgs() == 1) {
      const auto *ref = dyn_cast<DeclRefExpr>(expr->getArg(0)->IgnoreParenImpCasts());
      if (ref && objects.count(dyn_cast<VarDecl>(ref->getDecl()))) {
        reads.emplace(expr, reads.size() + 1);
        return true;
      }
    }
    facts_pb::EntityId calleeId;
    computeFuncEntityId(expr->getConstructor(), ctx, ctx.getSourceManager(),
                        &calleeId);
    effectCallees[{calleeId.id_hi(), calleeId.id()}] = calleeId;
    if (!constructions.count(expr))
      ownIsolated = false;
    const auto *constructor = expr->getConstructor();
    for (unsigned i = 0; i < constructor->getNumParams(); ++i)
      if (constructor->getParamDecl(i)->getType()->isReferenceType()) {
        if (i < expr->getNumArgs())
          expose(expr->getArg(i));
        else
          poisonAll = true;
      }
    return true;
  }
  bool VisitCXXNewExpr(CXXNewExpr *) {
    complete = false;
    ownIsolated = false;
    return true;
  }
  bool VisitImplicitCastExpr(ImplicitCastExpr *expr) {
    if (expr->getCastKind() == CK_LValueToRValue)
      reads.emplace(expr, reads.size() + 1);
    return true;
  }
  Value initializedAddress(const Expr *expr, unsigned depth) {
    Value out;
    if (!expr || depth > 128) return out;
    expr = expr->IgnoreParenImpCasts();
    if (const auto *ref = dyn_cast<DeclRefExpr>(expr)) {
      if (isa<FunctionDecl>(ref->getDecl()) || (isa<VarDecl>(ref->getDecl()) && !ref->getDecl()->getType()->isReferenceType())) out.set_kind(Value::IV_DEFINED);
    } else if (isa<StringLiteral>(expr)) out.set_kind(Value::IV_DEFINED);
    else if (const auto *member = dyn_cast<MemberExpr>(expr)) {
      const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
      if (field && !field->getType()->isReferenceType()) return member->isArrow() ? value(member->getBase(), depth + 1) : initializedAddress(member->getBase(), depth + 1);
    } else if (const auto *element = dyn_cast<ArraySubscriptExpr>(expr)) {
      out.set_kind(Value::IV_COMBINE);
      *out.add_operands() = value(element->getBase(), depth + 1);
      *out.add_operands() = value(element->getIdx(), depth + 1);
    } else if (const auto *unary = dyn_cast<UnaryOperator>(expr)) {
      if (unary->getOpcode() == UO_Deref) return value(unary->getSubExpr(), depth + 1);
    }
    return out;
  }
  Value value(const Expr *expr, unsigned depth = 0) {
    Value out;
    out.set_unknown_reason(!expr ? "no-initializer" : depth > 128 ? "expression-budget-exhausted" : "unsupported-value-expression");
    if (!expr || depth > 128)
      return out;
    expr = expr->IgnoreParens();
    if (const auto *cleanups = dyn_cast<ExprWithCleanups>(expr)) {
      if (!cleanups->cleanupsHaveSideEffects()) return value(cleanups->getSubExpr(), depth + 1);
      return out;
    }

    if (supportedRecord(expr->getType())) {
      if (const auto *temporary = dyn_cast<MaterializeTemporaryExpr>(expr)) return value(temporary->getSubExpr(), depth + 1);
      if (const auto *copy = dyn_cast<CXXConstructExpr>(expr)) {
        if (copy->getConstructor()->isTrivial() && copy->getConstructor()->isCopyOrMoveConstructor() && copy->getNumArgs() == 1)
          return value(copy->getArg(0), depth + 1);
      }
      if (const auto *cast = dyn_cast<ImplicitCastExpr>(expr)) {
        if (cast->getCastKind() == CK_NoOp || cast->getCastKind() == CK_LValueToRValue) return value(cast->getSubExpr(), depth + 1);
      }
      if (const auto *ref = dyn_cast<DeclRefExpr>(expr)) {
        const auto *decl = dyn_cast<VarDecl>(ref->getDecl());
        auto object = objects.find(decl);
        if (object != objects.end() && !poisonAll && !escapedObjects.count(decl)) {
          out.set_kind(Value::IV_COMBINE);
          for (auto slot : object->second.fields) {
            auto *field = out.add_operands(); field->set_kind(Value::IV_LOCAL); field->set_index(slot);
          }
          return out;
        }
      }
      if (const auto *list = dyn_cast<InitListExpr>(expr)) {
        if (list->isSemanticForm() && list->getNumInits() == std::distance(supportedRecord(expr->getType())->field_begin(), supportedRecord(expr->getType())->field_end())) {
          out.set_kind(Value::IV_COMBINE);
          for (const auto *field : list->inits()) *out.add_operands() = value(field, depth + 1);
          return out;
        }
      }
    }
    if (const auto *field = receiverField(expr)) {
      if (poisonAll || !ownIsolated)
        return out;
      out.set_kind(Value::IV_RECEIVER_FIELD);
      out.set_index(field->getFieldIndex());
      return out;
    }
    if (const auto *field = constructorField(expr)) {
      if (poisonAll || !ownIsolated)
        return out;
      out.set_kind(Value::IV_CONSTRUCTOR_FIELD);
      out.set_index(field->getFieldIndex());
      return out;
    }
    uint32_t fieldSlot = 0;
    if (objectFieldSlot(expr, fieldSlot)) {
      if (!poisonAll && !escapedObjects.count(objectBase(expr))) {
        out.set_kind(Value::IV_LOCAL);
        out.set_index(fieldSlot);
      }
      return out;
    }
    if (const auto *member = dyn_cast<MemberExpr>(expr)) {
      if (!member->isArrow() && isa<FieldDecl>(member->getMemberDecl()) &&
          supportedRecord(member->getBase()->getType()) && tracked(member->getType()))
        return value(member->getBase(), depth + 1);
    }
    if (const auto *alias = aliasStorage(expr)) {
      auto found = locals.find(alias);
      if (found != locals.end() && !poisonAll && !escaped.count(alias)) { out.set_kind(Value::IV_LOCAL); out.set_index(found->second); }
      return out;
    }
    const int input = readonlyStorage(expr);
    if (input >= 0) {
      if (!poisonAll && ownIsolated) { out.set_kind(Value::IV_BORROWED_INPUT); out.set_index(input); }
      return out;
    }
    if (outputStorage(expr)) {
      out.set_kind(Value::IV_OUTPUT);
      return out;
    }
    if (const auto *construction = dyn_cast<CXXConstructExpr>(expr)) {
      auto copy = copies.find(construction);
      if (copy == copies.end() || poisonAll || escapedObjects.count(copy->second)) return out;
      out.set_kind(Value::IV_COMBINE);
      for (auto slot : objects.find(copy->second)->second.fields) {
        auto *field = out.add_operands();
        field->set_kind(Value::IV_LOCAL);
        field->set_index(slot);
      }
      return out;
    }
    if (const auto *initializer = dyn_cast<CXXDefaultInitExpr>(expr))
      return value(initializer->getExpr(), depth + 1);
    if (const auto *constant = dyn_cast<ConstantExpr>(expr))
      return value(constant->getSubExpr(), depth + 1);
    if (const auto *argument = dyn_cast<CXXDefaultArgExpr>(expr))
      return value(argument->getExpr(), depth + 1);
    if (initializationScalar(expr->getType()) &&
        isa<CXXScalarValueInitExpr, ImplicitValueInitExpr>(expr)) {
      out.set_kind(Value::IV_DEFINED);
      return out;
    }
    if (const auto *list = dyn_cast<InitListExpr>(expr)) {
      if (!tracked(list->getType()))
        return out;
      if (list->getNumInits() == 0) {
        out.set_kind(Value::IV_DEFINED);
        return out;
      }
      if (list->getNumInits() == 1)
        return value(list->getInit(0), depth + 1);
      return out;
    }
    if (const auto *cast = dyn_cast<CastExpr>(expr)) {
      if (cast->getCastKind() == CK_NullToPointer || cast->getCastKind() == CK_FunctionToPointerDecay) { out.set_kind(Value::IV_DEFINED); return out; }
      if (cast->getCastKind() == CK_ArrayToPointerDecay) return initializedAddress(cast->getSubExpr(), depth + 1);
      if (initializationScalar(cast->getType()) && initializationScalar(cast->getSubExpr()->getType()))
        return value(cast->getSubExpr(), depth + 1);
      return out;
    }
    if (isa<IntegerLiteral, FloatingLiteral, CharacterLiteral,
            CXXBoolLiteralExpr, CXXNullPtrLiteralExpr>(expr)) {
      out.set_kind(Value::IV_DEFINED);
    } else if (const auto *ref = dyn_cast<DeclRefExpr>(expr)) {
      const auto *decl = dyn_cast<VarDecl>(ref->getDecl());
      auto found = locals.find(decl);
      out.set_unknown_reason(poisonAll ? "opaque-memory-effects" : escaped.count(decl) ? "escaped-storage" : "untracked-storage");
      if (found != locals.end() && !poisonAll && !escaped.count(decl)) {
        out.set_kind(Value::IV_LOCAL);
        out.set_index(found->second);
      } else if (isa<EnumConstantDecl>(ref->getDecl())) {
        out.set_kind(Value::IV_DEFINED);
      }
    } else if (const auto *call = dyn_cast<CallExpr>(expr)) {
      auto *callee = exactDirectCallee(call);
      auto getter = getterCalls.find(call);
      if (getter != getterCalls.end()) {
        if (poisonAll || escapedObjects.count(getter->second))
          return out;
        const auto &object = objects.find(getter->second)->second;
        for (auto local : object.fields)
          out.add_receiver_locals(local);
        out.set_receiver_record_identity(recordIdentity(object.record, ctx));
        auto argument = recordArgumentCalls.find(call);
        if (argument != recordArgumentCalls.end()) {
          out.set_receiver_is_parameter(true);
          out.set_receiver_parameter_index(argument->second);
        }
      }
      if (!callee || (!initializationScalar(call->getType()) && !supportedRecord(call->getType())) ||
          (isa<CXXMethodDecl>(callee) && getter == getterCalls.end()) ||
          callee->isVariadic() || call->getNumArgs() != callee->getNumParams())
        return out;
      out.set_kind(call->getType()->isPointerType() ? Value::IV_POINTER_CALL : Value::IV_CALL);
      if (const auto *record = supportedRecord(call->getType())) {
        for (const auto *argument : call->arguments()) if (argument->HasSideEffects(ctx)) return Value{};
        out.set_kind(Value::IV_RECORD_CALL);
        out.set_return_record_identity(recordIdentity(record, ctx));
      }
      bool recordParameter = false;
      for (const auto *parameter : callee->parameters()) recordParameter |= supportedRecord(parameter->getType()) != nullptr;
      if (recordParameter)
        for (const auto *parameter : callee->parameters()) {
          const auto *record = supportedRecord(parameter->getType());
          out.add_argument_record_identities(record ? recordIdentity(record, ctx) : "");
        }
      const auto binding = borrowedCalls.find(call);
      if (binding != borrowedCalls.end()) {
        out.set_borrowed_argument_index(binding->second.argument);
        auto *actual = out.mutable_borrowed_actual();
        if (binding->second.parameter) actual->set_kind(Value::IV_OUTPUT);
        else {
          auto slot = locals.find(binding->second.local);
          if (slot != locals.end() && !poisonAll && !escaped.count(binding->second.local)) {
            actual->set_kind(Value::IV_LOCAL);
            actual->set_index(slot->second);
          }
        }
      }
      auto inputs = readonlyCalls.find(call);
      if (inputs != readonlyCalls.end()) {
        for (unsigned i = 0; i < call->getNumArgs(); ++i) out.add_readonly_actuals();
        for (const auto &input : inputs->second) *out.mutable_readonly_actuals(input.argument) = borrowedValue(input);
      }
      computeFuncEntityId(const_cast<FunctionDecl *>(callee), ctx,
                          ctx.getSourceManager(), out.mutable_callee());
      for (unsigned i = 0; i < call->getNumArgs(); ++i) {
        auto *argument = out.add_operands();
        auto receiver = recordArgumentCalls.find(call);
        if (receiver == recordArgumentCalls.end() || receiver->second != i)
          *argument = value(call->getArg(i), depth + 1);
      }
    } else if (const auto *binary = dyn_cast<BinaryOperator>(expr)) {
      if (!binary->isAssignmentOp() && initializationScalar(binary->getType())) {
        out.set_kind(Value::IV_COMBINE);
        *out.add_operands() = value(binary->getLHS(), depth + 1);
        *out.add_operands() = value(binary->getRHS(), depth + 1);
      }
    } else if (const auto *unary = dyn_cast<UnaryOperator>(expr)) {
      if (unary->getOpcode() == UO_AddrOf) return initializedAddress(unary->getSubExpr(), depth + 1);
      if (unary->getOpcode() == UO_Plus || unary->getOpcode() == UO_Minus ||
          unary->getOpcode() == UO_Not || unary->getOpcode() == UO_LNot)
        return value(unary->getSubExpr(), depth + 1);
    } else if (const auto *conditional = dyn_cast<ConditionalOperator>(expr)) {
      out.set_kind(Value::IV_COMBINE);
      *out.add_operands() = value(conditional->getCond(), depth + 1);
      *out.add_operands() = value(conditional->getTrueExpr(), depth + 1);
      *out.add_operands() = value(conditional->getFalseExpr(), depth + 1);
    }
    return out;
  }
  Value borrowedValue(const BorrowedCall &binding) {
    Value actual;
    if (binding.parameter) actual.set_kind(Value::IV_OUTPUT);
    else if (binding.readonlyParameter) { actual.set_kind(Value::IV_BORROWED_INPUT); actual.set_index(binding.parameterIndex); }
    else {
      auto slot = locals.find(binding.local);
      if (slot != locals.end() && !poisonAll && !escaped.count(binding.local)) { actual.set_kind(Value::IV_LOCAL); actual.set_index(slot->second); }
    }
    return actual;
  }
  std::map<const VarDecl *, const VarDecl *> aliases;
  std::set<const VarDecl *> reboundBorrowedPointers;
  std::map<const ParmVarDecl *, unsigned> readonlyParameters;
  std::map<const CallExpr *, std::vector<BorrowedCall>> readonlyCalls;
  FunctionDecl *function;
  ASTContext &ctx;
  bool complete = true;
  bool ownIsolated = true;
  const ParmVarDecl *recordOutput = nullptr;
  unsigned recordOutputIndex = 0;
  std::map<const CallExpr *, std::pair<const VarDecl *, unsigned>> recordOutputCalls;
  const CXXRecordDecl *constructorRecord = nullptr;
  const CXXRecordDecl *receiverRecord = nullptr;
  const ParmVarDecl *receiverParameter = nullptr;
  unsigned receiverParameterIndex = 0;
  std::map<const CallExpr *, const VarDecl *> getterCalls;
  std::map<const CallExpr *, unsigned> recordArgumentCalls;
  uint32_t localCount = 0;
  std::map<const VarDecl *, Object> objects;
  std::map<const CXXConstructExpr *, const VarDecl *> constructions;
  std::map<const CXXConstructExpr *, const VarDecl *> copies;
  std::set<const VarDecl *> escapedObjects;
  const ParmVarDecl *output = nullptr;
  unsigned outputIndex = 0;
  std::map<const CallExpr *, BorrowedCall> borrowedCalls;
  std::set<const UnaryOperator *> borrowedAddresses;
  std::map<std::pair<uint64_t, uint64_t>, facts_pb::EntityId> effectCallees;
  std::map<const CallExpr *, const VarDecl *> recordAssignments;
  unsigned nodes = 0;
  bool poisonAll = false;
  std::set<const CastExpr *> borrowedCasts;
  std::set<const VarDecl *> escaped;
  std::map<const VarDecl *, uint32_t> locals;
  std::map<const Expr *, uint64_t> reads;
};
// Independent pointer-origin must proof. Unknown pointee contents never inherit
// the provenance of the pointer used to access them.
std::map<const Expr *, std::pair<facts_pb::EntityId, uint64_t>> originAnchors;
std::set<const Expr *> ambiguousOriginAnchors;
class PointerOrigins : public RecursiveASTVisitor<PointerOrigins> {
 public:
  ASTContext &ctx;
  bool complete = true;
  bool pointerCall = false;
  unsigned nodes = 0;
  uint32_t localCount = 0;
  std::map<const VarDecl *, uint32_t> locals;
  using FieldPath = std::vector<const FieldDecl *>;
  std::map<std::pair<const VarDecl *, FieldPath>, uint32_t> fields;
  std::set<const VarDecl *> records;
  std::set<const VarDecl *> escaped;
  std::map<const Expr *, const Expr *> reads;
  std::map<std::pair<uint64_t, uint64_t>, facts_pb::EntityId> callees;
  explicit PointerOrigins(ASTContext &context) : ctx(context) {}
  const VarDecl *variable(const Expr *e) const {
    const auto *ref = e ? dyn_cast<DeclRefExpr>(e->IgnoreParenImpCasts()) : nullptr;
    return ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
  }
  const VarDecl *fieldOwner(const Expr *e, FieldPath *path = nullptr) const {
    FieldPath reversed;
    while (e && reversed.size() < 8) {
      e = e->IgnoreParenImpCasts();
      const auto *member = dyn_cast<MemberExpr>(e);
      if (!member) break;
      const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
      if (member->isArrow() || !field || field->getType()->isReferenceType()) return nullptr;
      reversed.push_back(field);
      e = member->getBase();
    }
    const auto *v = variable(e);
    if (reversed.empty() || !v || !records.count(v)) return nullptr;
    if (path) path->assign(reversed.rbegin(), reversed.rend());
    return v;
  }
  bool collectFields(QualType type, FieldPath path, std::vector<FieldPath> &result, unsigned &count) const {
    if (++count > 512 || path.size() > 8 || type.isVolatileQualified() || type->isReferenceType()) return false;
    const auto *record = type->getAsCXXRecordDecl();
    if (!record || !record->hasDefinition()) return false;
    record = record->getDefinition();
    if (!record->isTrivial() || !record->isTriviallyCopyable() || record->getNumBases()) return false;
    for (const auto *field : record->fields()) {
      auto next = path;
      next.push_back(field);
      if (field->getType()->isReferenceType()) return false;
      if (field->getType().isVolatileQualified()) continue;
      if (field->getType()->isPointerType()) result.push_back(std::move(next));
      else if (field->getType()->isRecordType() && !collectFields(field->getType(), std::move(next), result, count)) return false;
      if (result.size() > 512) return false;
    }
    return true;
  }
  static bool overlaps(const FieldPath &left, const FieldPath &right) {
    for (unsigned i = 0; i < left.size() && i < right.size(); ++i)
      if (left[i] != right[i]) return left[i]->getParent() == right[i]->getParent() && left[i]->getParent()->isUnion();
    return true; // Same field, or an enclosing subobject write.
  }
  bool slot(const Expr *e, uint32_t &index) const {
    FieldPath path;
    if (const auto *v = fieldOwner(e, &path)) {
      const auto found = fields.find({v, path});
      if (found == fields.end() || escaped.count(v) || e->getType().isVolatileQualified()) return false;
      index = found->second;
      return true;
    }
    const auto *v = variable(e);
    const auto found = locals.find(v);
    if (found == locals.end() || escaped.count(v)) return false;
    index = found->second;
    return true;
  }
  void expose(const Expr *e) {
    if (const auto *v = variable(e)) escaped.insert(v);
    else if (const auto *v = fieldOwner(e)) escaped.insert(v);
    else if (e && e->getType()->isPointerType()) complete = false;
  }
  bool VisitStmt(Stmt *s) {
    if (++nodes > 8192) { complete = false; return false; }
    if (isa<AsmStmt, IndirectGotoStmt, CXXTryStmt, SEHTryStmt,
            LambdaExpr, CoroutineBodyStmt, StmtExpr, CXXParenListInitExpr>(s)) complete = false;
    return true;
  }
  bool VisitVarDecl(VarDecl *v) {
    if (v->hasLocalStorage() && v->getType()->isPointerType() &&
        !v->getType().isVolatileQualified() && !v->hasAttr<CleanupAttr>())
      locals.emplace(v, localCount++);
    // Field values have their own provenance slots; the record's address is
    // never used as the provenance of a pointer loaded from that record.
    if (v->hasLocalStorage() && !isa<ParmVarDecl>(v) && !v->hasAttr<CleanupAttr>()) {
      std::vector<FieldPath> paths;
      unsigned count = 0;
      if (collectFields(v->getType(), {}, paths, count)) {
        records.insert(v);
        for (auto &path : paths) fields.emplace(std::make_pair(v, std::move(path)), localCount++);
      }
    }
    if (v->getType()->isReferenceType() && v->hasInit() &&
        (v->getType()->getPointeeType()->isPointerType() || fieldOwner(v->getInit()))) expose(v->getInit());
    return true;
  }
  bool VisitDeclRefExpr(DeclRefExpr *e) {
    const auto *v = dyn_cast<VarDecl>(e->getDecl());
    if (!records.count(v)) return true;
    const Expr *node = e;
    for (unsigned depth = 0; depth != 32; ++depth) {
      auto parents = ctx.getParents(*node);
      if (parents.size() != 1) break;
      if (const auto *paren = parents[0].get<ParenExpr>()) { node = paren; continue; }
      if (const auto *member = parents[0].get<MemberExpr>())
        if (!member->isArrow() && member->getBase() == node && isa<FieldDecl>(member->getMemberDecl())) return true;
      if (parents[0].get<UnaryExprOrTypeTraitExpr>()) return true;
      break;
    }
    // Whole-object copies, reference binding, address publication, and custom
    // operations require field-sensitive transfer/escape summaries.
    escaped.insert(v);
    return true;
  }
  bool VisitImplicitCastExpr(ImplicitCastExpr *e) {
    if (e->getCastKind() == CK_LValueToRValue && e->getType()->isPointerType()) reads[e] = e->getSubExpr();
    return true;
  }
  bool VisitCastExpr(CastExpr *e) {
    if (e->getCastKind() == CK_IntegralToPointer) complete = false;
    if (e->getCastKind() == CK_ArrayToPointerDecay && fieldOwner(e->getSubExpr())) expose(e->getSubExpr());
    // A reference cast can export the pointer slot through a different type,
    // without an address-of expression (for example uintptr_t&(pointer)).
    if (e->isGLValue() && (e->getSubExpr()->getType()->isPointerType() || fieldOwner(e->getSubExpr()))) expose(e->getSubExpr());
    return true;
  }
  bool VisitInitListExpr(InitListExpr *e) {
    // Aggregate reference members can retain a pointer slot. Ordinary pointer
    // copies have an lvalue-to-rvalue conversion and do not enter this branch.
    for (const auto *init : e->inits())
      if (init && init->isGLValue() && (init->getType()->isPointerType() || fieldOwner(init))) expose(init);
    return true;
  }
  bool VisitMemberExpr(MemberExpr *e) {
    if (e->isArrow()) reads[e] = e->getBase();
    return true;
  }
  bool VisitArraySubscriptExpr(ArraySubscriptExpr *e) { reads[e] = e->getBase(); return true; }
  bool VisitUnaryOperator(UnaryOperator *e) {
    if (e->getOpcode() == UO_AddrOf && (e->getSubExpr()->getType()->isPointerType() || fieldOwner(e->getSubExpr()))) expose(e->getSubExpr());
    if (e->isIncrementDecrementOp() && fieldOwner(e->getSubExpr()) && !standalone(e)) complete = false;
    if (e->getOpcode() == UO_Deref || (e->isIncrementDecrementOp() && e->getType()->isPointerType())) reads[e] = e->getSubExpr();
    return true;
  }
  bool standalone(const Expr *e) const {
    const Stmt *s = e;
    for (unsigned depth = 0; depth != 32; ++depth) {
      auto parents = ctx.getParents(*s);
      if (parents.size() != 1) return false;
      if (const auto *p = parents[0].get<ParenExpr>()) { s = p; continue; }
      if (parents[0].get<CompoundStmt>() || parents[0].get<LabelStmt>() || parents[0].get<SwitchCase>()) return true;
      if (const auto *loop = parents[0].get<ForStmt>()) return loop->getInc() == s || loop->getBody() == s;
      if (const auto *branch = parents[0].get<IfStmt>()) return branch->getThen() == s || branch->getElse() == s;
      if (const auto *loop = parents[0].get<WhileStmt>()) return loop->getBody() == s;
      if (const auto *loop = parents[0].get<DoStmt>()) return loop->getBody() == s;
      return false;
    }
    return false;
  }
  bool VisitBinaryOperator(BinaryOperator *e) {
    if (e->isAssignmentOp() && fieldOwner(e->getLHS()) && !standalone(e)) complete = false;
    if (e->isAssignmentOp() && e->getLHS()->getType()->isPointerType()) {
      if (!standalone(e)) complete = false;
      if (e->isCompoundAssignmentOp()) reads[e] = e->getLHS();
    }
    return true;
  }
  bool VisitCallExpr(CallExpr *call) {
    const auto *callee = exactDirectCallee(call);
    if (const auto *assignment = dyn_cast<CXXOperatorCallExpr>(call)) {
      const auto *method = dyn_cast_or_null<CXXMethodDecl>(callee);
      // Trivial record assignment cannot mutate an unescaped pointer local.
      // Record fields remain unknown in this channel; do not discard all
      // scalar-pointer facts merely because the function also copies a record.
      if (method && method->isTrivial() && assignment->getNumArgs() == 2 &&
          (method->isCopyAssignmentOperator() || method->isMoveAssignmentOperator())) {
        for (const auto *argument : assignment->arguments())
          if (fieldOwner(argument)) expose(argument);
        return true;
      }
    }
    if (!callee || isa<CXXMethodDecl>(callee) || callee->isVariadic() || call->getNumArgs() != callee->getNumParams()) {
      // Unknown calls cannot mutate an unescaped local pointer, but argument
      // reference binding is unknown too. Keep this first model conservative.
      complete = false;
      return true;
    }
    for (const auto *parameter : callee->parameters())
      pointerCall = pointerCall || parameter->getType()->isPointerType();
    facts_pb::EntityId id;
    computeFuncEntityId(const_cast<FunctionDecl *>(callee), ctx, ctx.getSourceManager(), &id);
    callees[{id.id_hi(), id.id()}] = id;
    for (unsigned i = 0; i < callee->getNumParams(); ++i)
      if (callee->getParamDecl(i)->getType()->isReferenceType() &&
          (call->getArg(i)->getType()->isPointerType() || fieldOwner(call->getArg(i)))) expose(call->getArg(i));
    if (callee->hasAttr<ReturnsTwiceAttr>()) complete = false;
    if (callee->getIdentifier()) {
      const auto name = callee->getName();
      if (name == "setjmp" || name == "_setjmp" || name == "sigsetjmp" || name == "__sigsetjmp" || name == "__builtin_setjmp") complete = false;
    }
    return true;
  }
  bool VisitCXXConstructExpr(CXXConstructExpr *e) {
    const auto *callee = e->getConstructor();
    if (!callee || e->getNumArgs() != callee->getNumParams()) { complete = false; return true; }
    for (unsigned i = 0; i < callee->getNumParams(); ++i)
      if (callee->getParamDecl(i)->getType()->isReferenceType() &&
          (e->getArg(i)->getType()->isPointerType() || fieldOwner(e->getArg(i)))) expose(e->getArg(i));
    return true;
  }
  Value address(const Expr *e, unsigned depth) {
    Value out;
    if (!e || depth > 64) return out;
    e = e->IgnoreParenImpCasts();
    if (const auto *v = variable(e)) {
      if (!v->getType()->isReferenceType()) out.set_kind(Value::IV_DEFINED);
    } else if (const auto *member = dyn_cast<MemberExpr>(e)) {
      const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
      if (field && !field->getType()->isReferenceType())
        return member->isArrow() ? value(member->getBase(), depth + 1) : address(member->getBase(), depth + 1);
    } else if (const auto *element = dyn_cast<ArraySubscriptExpr>(e)) return value(element->getBase(), depth + 1);
    else if (const auto *unary = dyn_cast<UnaryOperator>(e)) {
      if (unary->getOpcode() == UO_Deref) return value(unary->getSubExpr(), depth + 1);
    } else if (isa<StringLiteral>(e)) out.set_kind(Value::IV_DEFINED);
    return out;
  }
  Value value(const Expr *e, unsigned depth = 0) {
    Value out;
    out.set_unknown_reason("pointer-origin-unresolved");
    if (!e || depth > 64 || e->isInstantiationDependent() || e->getType().isNull()) return out;
    e = e->IgnoreParens();
    if (const auto *cast = dyn_cast<CastExpr>(e)) {
      if (cast->getCastKind() == CK_ArrayToPointerDecay) return address(cast->getSubExpr(), depth + 1);
      if (cast->getCastKind() == CK_NullToPointer || cast->getCastKind() == CK_FunctionToPointerDecay) { out.set_kind(Value::IV_DEFINED); return out; }
      if (cast->getType()->isFunctionPointerType() || cast->getSubExpr()->getType()->isFunctionPointerType()) {
            if (!ctx.hasSameType(cast->getType(),cast->getSubExpr()->getType())) return out;
          }
          if (cast->getType()->isPointerType() && cast->getSubExpr()->getType()->isPointerType()) return value(cast->getSubExpr(), depth + 1);
      return out;
    }
    if (isa<StringLiteral, CXXNullPtrLiteralExpr>(e)) { out.set_kind(Value::IV_DEFINED); return out; }
    uint32_t index = 0;
    if (slot(e, index)) {
      out.set_kind(Value::IV_LOCAL); out.set_index(index);
      return out;
    }
    if (const auto *unary = dyn_cast<UnaryOperator>(e)) {
      if (unary->getOpcode() == UO_AddrOf) return address(unary->getSubExpr(), depth + 1);
    }
    if (const auto *binary = dyn_cast<BinaryOperator>(e)) {
      if ((binary->getOpcode() == BO_Add || binary->getOpcode() == BO_Sub) && !binary->HasSideEffects(ctx)) {
        if (binary->getLHS()->getType()->isPointerType() && binary->getRHS()->getType()->isIntegerType()) return value(binary->getLHS(), depth + 1);
        if (binary->getOpcode() == BO_Add && binary->getRHS()->getType()->isPointerType() && binary->getLHS()->getType()->isIntegerType()) return value(binary->getRHS(), depth + 1);
      }
    }
    if (const auto *conditional = dyn_cast<ConditionalOperator>(e)) {
      if (!conditional->HasSideEffects(ctx)) {
        out.set_kind(Value::IV_COMBINE);
        *out.add_operands() = value(conditional->getTrueExpr(), depth + 1);
        *out.add_operands() = value(conditional->getFalseExpr(), depth + 1);
      }
      return out;
    }
    if (const auto *call = dyn_cast<CallExpr>(e)) {
      const auto *callee = exactDirectCallee(call);
      if (!callee || isa<CXXMethodDecl>(callee) || !call->getType()->isPointerType() || callee->isVariadic() || call->getNumArgs() != callee->getNumParams()) return out;
      for (const auto *argument : call->arguments()) if (argument->HasSideEffects(ctx)) return out;
      if (callee->isExternC() && callee->getIdentifier() && ctx.getSourceManager().isInSystemHeader(callee->getLocation())) {
        const auto name = callee->getName();
        if (name == "malloc" || name == "calloc" || name == "realloc" || name == "aligned_alloc") { out.set_kind(Value::IV_DEFINED); return out; }
      }
      out.set_kind(Value::IV_CALL);
      computeFuncEntityId(const_cast<FunctionDecl *>(callee), ctx, ctx.getSourceManager(), out.mutable_callee());
      for (const auto *argument : call->arguments()) *out.add_operands() = value(argument, depth + 1);
    }
    return out;
  }
};
void buildPointerOrigins(FunctionDecl *fn, ASTContext &ctx, facts_pb::InitializationFunction *parent) {
  if (isa<CXXMethodDecl>(fn) || fn->isVariadic() || fn->isDependentContext()) return;
  auto *out = parent->mutable_pointer_origin();
  out->set_version(8);
  *out->mutable_function() = parent->function();
  *out->mutable_loc() = parent->loc();
  out->set_definition_hash(parent->definition_hash());
  out->set_parameter_count(fn->getNumParams());
  out->set_scalar_return(fn->getReturnType()->isPointerType());
  out->set_closed_callers(parent->closed_callers());
  *out->mutable_callers() = parent->callers();
  PointerOrigins flow(ctx);
  for (auto *parameter : fn->parameters()) flow.VisitVarDecl(parameter);
  flow.TraverseStmt(fn->getBody());
  if (!flow.complete || (flow.reads.empty() && !flow.pointerCall && !fn->getReturnType()->isPointerType())) {
    parent->clear_pointer_origin();
    return;
  }
  out->set_local_count(flow.localCount);
  for (const auto &entry : flow.callees) *out->add_effect_callees() = entry.second;
  // Dependencies participate in the existing distributed fan-out as well.
  std::set<std::pair<uint64_t, uint64_t>> dependencies;
  for (const auto &callee : parent->effect_callees()) dependencies.emplace(callee.id_hi(), callee.id());
  for (const auto &entry : flow.callees) if (dependencies.insert(entry.first).second) *parent->add_effect_callees() = entry.second;
  if (!flow.complete || flow.localCount > 512 || fn->getNumParams() > 128) return;
  CFG::BuildOptions options;
  options.setAllAlwaysAdd();
  auto cfg = CFG::buildCFG(fn, fn->getBody(), &ctx, options);
  if (!cfg || cfg->size() > 512) return;
  std::map<const Expr *, uint64_t> emitted;
  for (const auto *block : *cfg) {
    auto *target = out->add_blocks();
    target->set_block_id(block->getBlockID());
    target->set_is_entry(block == &cfg->getEntry());
    if (block == &cfg->getEntry()) for (unsigned i = 0; i < fn->getNumParams(); ++i) {
      auto found = flow.locals.find(fn->getParamDecl(i));
      if (found == flow.locals.end()) continue;
      auto *op = target->add_operations(); op->set_kind(Operation::IO_ASSIGN); op->set_local(found->second);
      op->mutable_value()->set_kind(Value::IV_PARAMETER); op->mutable_value()->set_index(i);
    }
    std::set<uint32_t> successors;
    for (auto it = block->succ_begin(); it != block->succ_end(); ++it) if (const auto *next = it->getReachableBlock()) successors.insert(next->getBlockID());
    for (auto successor : successors) target->add_successors(successor);
    for (const auto &element : *block) {
      auto item = element.getAs<CFGStmt>(); if (!item) continue;
      const auto *stmt = item->getStmt();
      if (const auto *expr = dyn_cast<Expr>(stmt)) {
        auto read = flow.reads.find(expr);
        if (read != flow.reads.end()) {
          if (emitted.count(expr)) { flow.complete = false; continue; }
          uint64_t id = (uint64_t(1) << 63) | (emitted.size() + 1);
          emitted.emplace(expr, id);
          auto *op = target->add_operations(); op->set_kind(Operation::IO_READ); op->set_obligation_id(id);
          *op->mutable_value() = flow.value(read->second);
          setLoc(op->mutable_loc(), expr->getExprLoc(), ctx.getSourceManager());
        }
      }
      auto assign = [&](const VarDecl *v, const Expr *value) {
        auto found = flow.locals.find(v); if (found == flow.locals.end()) return;
        auto *op = target->add_operations(); op->set_kind(Operation::IO_ASSIGN); op->set_local(found->second);
        *op->mutable_value() = flow.value(value);
      };
      auto invalidateOverlaps = [&](const Expr *written) {
        PointerOrigins::FieldPath path;
        const auto *owner = flow.fieldOwner(written, &path);
        if (!owner) return;
        for (const auto &entry : flow.fields) {
          if (entry.first.first != owner || entry.first.second == path || !PointerOrigins::overlaps(path, entry.first.second)) continue;
          auto *op = target->add_operations(); op->set_kind(Operation::IO_ASSIGN); op->set_local(entry.second);
          op->mutable_value()->set_unknown_reason("overlapping-union-member-write");
        }
      };
      if (const auto *decls = dyn_cast<DeclStmt>(stmt)) {
        for (const auto *decl : decls->decls()) if (const auto *v = dyn_cast<VarDecl>(decl)) {
          assign(v, v->getInit());
          // Every declaration begins a new storage epoch, including a local
          // recreated on a loop backedge. Do not inherit an old field's value.
          for (const auto &entry : flow.fields) if (entry.first.first == v) {
            auto *op = target->add_operations(); op->set_kind(Operation::IO_ASSIGN); op->set_local(entry.second);
            op->mutable_value()->set_unknown_reason("pointer-field-initializer-unresolved");
          }
        }
      } else if (const auto *binary = dyn_cast<BinaryOperator>(stmt)) {
        if (binary->isAssignmentOp()) invalidateOverlaps(binary->getLHS());
        if (binary->getOpcode() == BO_Assign) {
          uint32_t index = 0;
          if (flow.slot(binary->getLHS(), index)) {
            auto *op = target->add_operations(); op->set_kind(Operation::IO_ASSIGN); op->set_local(index);
            *op->mutable_value() = flow.value(binary->getRHS());
          }
        }
      } else if (const auto *unary = dyn_cast<UnaryOperator>(stmt)) {
        if (unary->isIncrementDecrementOp()) invalidateOverlaps(unary->getSubExpr());
      } else if (const auto *ret = dyn_cast<ReturnStmt>(stmt)) {
        auto *op = target->add_operations(); op->set_kind(Operation::IO_RETURN); *op->mutable_value() = flow.value(ret->getRetValue());
      } else if (const auto *call = dyn_cast<CallExpr>(stmt)) {
        const auto *callee = exactDirectCallee(call);
        if (!callee || isa<CXXMethodDecl>(callee) || callee->isVariadic() || call->getNumArgs() != callee->getNumParams()) continue;
        const auto census = callerCensus.find(callee->getCanonicalDecl());
        if (censusOpaque || census == callerCensus.end() || census->second.escaped) continue;
        auto *op = target->add_operations(); op->set_kind(Operation::IO_OBSERVE_CALL);
        computeFuncEntityId(const_cast<FunctionDecl *>(callee), ctx, ctx.getSourceManager(), op->mutable_callee());
        for (const auto *argument : call->arguments()) *op->add_arguments() = argument->HasSideEffects(ctx) ? Value{} : flow.value(argument);
      }
    }
  }
  out->set_complete(flow.complete);
  if (!flow.complete) return;
  for (const auto &entry : emitted) {
    auto previous = originAnchors.find(entry.first);
    if (ambiguousOriginAnchors.count(entry.first)) continue;
    if (previous != originAnchors.end() && (previous->second.first.id() != parent->function().id() || previous->second.first.id_hi() != parent->function().id_hi() || previous->second.second != entry.second)) {
      originAnchors.erase(previous); ambiguousOriginAnchors.insert(entry.first);
    } else originAnchors[entry.first] = {parent->function(), entry.second};
  }
}

// Independent ordered memory facts. No lifetime or initialization candidate is
// discharged merely because these facts exist; consumers must validate CFGs,
// bind exact call arguments, and use the appropriate proof domain.
class MemoryFacts : public RecursiveASTVisitor<MemoryFacts> {
 public:
  using MValue = facts_pb::MemoryValue;
  using MOp = facts_pb::MemoryOperation;
  ASTContext &ctx;
  bool complete = true;
  unsigned nodes = 0;
  unsigned initializationNodes = 0;
  uint32_t copyTemporary = uint32_t(1)<<29;
  std::map<const VarDecl *, uint32_t> locals;
  // Reference bindings are aliases, not independent storage epochs. Keep
  // their immutable binding slots disjoint from call-result temporaries.
  std::map<const VarDecl *, uint32_t> references;
  std::map<const CallExpr *, uint32_t> calls;
  std::map<const Expr *, uint64_t> reads;
  std::map<const Expr *, uint64_t> expressionOrdinals;
  facts_pb::EntityId function;
  std::set<const CallExpr *> logicalCalls;
  std::map<const BinaryOperator *, uint64_t> arithmetic;
  std::map<const BinaryOperator *, uint64_t> deadStores;
  bool receiver = false;
  explicit MemoryFacts(ASTContext &context, bool hasReceiver = false) : ctx(context), receiver(hasReceiver) {}
  bool supportedMethod(const FunctionDecl *callee) const {
    const auto *method=dyn_cast_or_null<CXXMethodDecl>(callee);
    return method && method->isConst() && !method->isVolatile() && !method->isVirtual() &&
      !method->isStatic() && !method->isVariadic() && tracked(method->getReturnType()) &&
      supportedRecord(ctx.getRecordType(method->getParent()));
  }
  bool VisitStmt(Stmt *stmt) {
    if (++nodes > 8192) { complete = false; return false; }
    if (isa<AsmStmt, IndirectGotoStmt, CXXTryStmt, SEHTryStmt, LambdaExpr,
            CoroutineBodyStmt, StmtExpr, CXXNewExpr, CXXDeleteExpr>(stmt)) complete = false;
    if (const auto *expr = dyn_cast<Expr>(stmt)) {
      expressionOrdinals.emplace(expr,expressionOrdinals.size()+1);
      if (isa<AtomicExpr, VAArgExpr, CXXDynamicCastExpr, CXXThrowExpr>(expr)) complete = false;
      if (expr->HasSideEffects(ctx) && !isa<CallExpr, BinaryOperator, UnaryOperator,
          CXXConstructExpr, CastExpr, ParenExpr, ExprWithCleanups,
          CXXBindTemporaryExpr, MaterializeTemporaryExpr, InitListExpr,
          CXXDefaultArgExpr, CXXDefaultInitExpr, ConstantExpr,
          ConditionalOperator, BinaryConditionalOperator>(expr)) complete = false;
    }
    return true;
  }
  bool storageType(QualType type) const {
    if (type.isNull() || type->isIncompleteType() || type->isDependentType() ||
        type->isVariablyModifiedType() || type->isReferenceType()) return false;
    if (type->isScalarType()) return true;
    if (const auto *array = ctx.getAsConstantArrayType(type)) return storageType(array->getElementType());
    const auto *record = type->getAsCXXRecordDecl();
    return record && record->hasDefinition() && record->isTrivial() &&
        record->isTriviallyCopyable() && record->getNumBases() == 0;
  }
  bool VisitVarDecl(VarDecl *decl) {
    if (!decl->hasLocalStorage()) return true;
    if (const auto *parameter = dyn_cast<ParmVarDecl>(decl))
      if (parameter->getType()->isLValueReferenceType() &&
          storageType(parameter->getType()->getPointeeType())) return true;
    if (decl->getType()->isLValueReferenceType() && !decl->hasAttr<CleanupAttr>() &&
        storageType(decl->getType()->getPointeeType()) && decl->hasInit() && decl->getInit()->isLValue()) {
      if (references.size()+locals.size()>=512) {complete=false;return false;}
      references.emplace(decl,(uint32_t(1)<<30)|references.size());return true;
    }
    if (decl->hasAttr<CleanupAttr>() || !storageType(decl->getType())) { complete = false; return true; }
    if (locals.size()+references.size() >= 512) { complete = false; return false; }
    locals.emplace(decl, locals.size());
    return true;
  }
  bool VisitImplicitCastExpr(ImplicitCastExpr *expr) {
    if (expr->getCastKind() == CK_LValueToRValue) {
      reads.emplace(expr, reads.size()+1);
    }
    return true;
  }
  bool noUnionStoreAccess(const Stmt *stmt, unsigned depth = 0) const {
    if (!stmt || depth > 64) return false;
    if (const auto *member = dyn_cast<MemberExpr>(stmt))
      if (const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl()))
        if (field->getParent()->isUnion()) return false;
    for (const auto *child : stmt->children())
      if (child && !noUnionStoreAccess(child, depth+1)) return false;
    return true;
  }
  bool VisitBinaryOperator(BinaryOperator *expression) {
    const auto parents=ctx.getParents(*expression);
    const auto type=expression->getLHS()->getType();
    if (expression->getOpcode()==BO_Assign && parents.size()==1 && parents[0].get<CompoundStmt>() &&
        (type->isIntegerType() || type->isPointerType()) && !type.isVolatileQualified() &&
        !expression->getLHS()->HasSideEffects(ctx) && !expression->getRHS()->HasSideEffects(ctx) &&
        noUnionStoreAccess(expression->getLHS()))
      deadStores.emplace(expression,deadStores.size()+1);
    if (expression->getType()->isIntegerType() &&
        ((expression->getType()->isSignedIntegerType() &&
          (expression->getOpcode()==BO_Add || expression->getOpcode()==BO_Sub || expression->getOpcode()==BO_Mul)) ||
         expression->getOpcode()==BO_Div || expression->getOpcode()==BO_Rem || expression->isShiftOp()))
      arithmetic.emplace(expression,arithmetic.size()+1);
    if (expression->isLogicalOp())
      if (const auto *call = dyn_cast<CallExpr>(expression->getRHS()->IgnoreParenImpCasts()))
        logicalCalls.insert(call);
    return true;
  }
  bool VisitCallExpr(CallExpr *call) {
    if (calls.size() >= 16384) { complete = false; return false; }
    calls.emplace(call, calls.size());
    if (const auto *callee = call->getDirectCallee())
      if (callee->hasAttr<ReturnsTwiceAttr>()) complete = false;
    return true;
  }
  // Private volatile loads are evaluated by the memory domain. They are not
  // arbitrary argument mutations; calls, updates and cleanup effects still
  // require ordered argument snapshots before they can be admitted here.
  bool stableArgument(const Expr *expr, unsigned depth = 0, std::vector<facts_pb::EntityId> *requirements = nullptr, std::vector<uint32_t> *evaluatedCalls = nullptr) const {
    if (!expr || depth > 64 || expr->isInstantiationDependent()) return false;
    expr = expr->IgnoreParens();
    if (const auto *call = dyn_cast<CallExpr>(expr)) {
      const auto *callee = exactDirectCallee(call);
      if (!callee) {
        const auto found=calls.find(call);
        if (!evaluatedCalls || found==calls.end() ||
            !stableArgument(call->getCallee(),depth+1,requirements,evaluatedCalls)) return false;
        for (const auto *argument:call->arguments())
          if (!stableArgument(argument,depth+1,requirements,evaluatedCalls)) return false;
        evaluatedCalls->push_back(found->second);
        return true;
      }
      if (!requirements || !callee || (isa<CXXMethodDecl>(callee) && !supportedMethod(callee)) || callee->isVariadic() || call->getNumArgs()!=callee->getNumParams()) return false;
      facts_pb::EntityId id;
      computeFuncEntityId(const_cast<FunctionDecl *>(callee),ctx,ctx.getSourceManager(),&id);
      requirements->push_back(id);
      if (const auto *member=dyn_cast<CXXMemberCallExpr>(call))
        if (!stableArgument(member->getImplicitObjectArgument(),depth+1,requirements,evaluatedCalls)) return false;
      for (const auto *argument:call->arguments()) if (!stableArgument(argument,depth+1,requirements,evaluatedCalls)) return false;
      return true;
    }
    if (isa<IntegerLiteral, FloatingLiteral, CharacterLiteral, CXXBoolLiteralExpr,
            CXXNullPtrLiteralExpr, StringLiteral, DeclRefExpr, CXXThisExpr>(expr)) return true;
    if (isa<UnaryExprOrTypeTraitExpr>(expr)) return !expr->HasSideEffects(ctx);
    if (const auto *cast = dyn_cast<CastExpr>(expr)) {
      if (isa<CXXDynamicCastExpr>(cast)) return false;
      return stableArgument(cast->getSubExpr(), depth + 1, requirements,evaluatedCalls);
    }
    if (const auto *member = dyn_cast<MemberExpr>(expr)) return stableArgument(member->getBase(), depth + 1, requirements,evaluatedCalls);
    if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(expr)) return stableArgument(subscript->getBase(), depth + 1, requirements,evaluatedCalls) && stableArgument(subscript->getIdx(), depth + 1, requirements,evaluatedCalls);
    if (const auto *binary = dyn_cast<BinaryOperator>(expr)) return !binary->isAssignmentOp() && stableArgument(binary->getLHS(), depth + 1, requirements,evaluatedCalls) && stableArgument(binary->getRHS(), depth + 1, requirements,evaluatedCalls);
    if (const auto *unary = dyn_cast<UnaryOperator>(expr)) return !unary->isIncrementDecrementOp() && stableArgument(unary->getSubExpr(), depth + 1, requirements,evaluatedCalls);
    if (const auto *argument = dyn_cast<CXXDefaultArgExpr>(expr)) return stableArgument(argument->getExpr(), depth + 1, requirements,evaluatedCalls);
    if (const auto *constant = dyn_cast<ConstantExpr>(expr)) return stableArgument(constant->getSubExpr(), depth + 1, requirements,evaluatedCalls);
    if (const auto *cleanup = dyn_cast<ExprWithCleanups>(expr)) return !cleanup->cleanupsHaveSideEffects() && stableArgument(cleanup->getSubExpr(), depth + 1, requirements,evaluatedCalls);
    return false;
  }
  void integerType(MValue &value, QualType type) {
    if (!type.isNull() && (type->isIntegerType() || type->isEnumeralType())) {
      value.set_integer_bits(ctx.getTypeSize(type));
      value.set_integer_unsigned(type->isUnsignedIntegerOrEnumerationType());
    }
  }
  facts_pb::MemoryAccess access(QualType type, const Expr *expr = nullptr) {
    facts_pb::MemoryAccess out;
    if (!storageType(type) || !type->isScalarType()) return out;
    out.set_bytes(ctx.getTypeSizeInChars(type).getQuantity());
    out.set_is_volatile(type.isVolatileQualified());
    std::string identity = mangleType(type.getCanonicalType().getUnqualifiedType(), ctx);
    for (unsigned depth = 0; expr && depth < 16; ++depth) {
      expr = expr->IgnoreParenImpCasts();
      const auto *member = dyn_cast<MemberExpr>(expr);
      const auto *field = member ? dyn_cast<FieldDecl>(member->getMemberDecl()) : nullptr;
      if (!field) break;
      if (field->getParent()->isUnion()) {
        const auto *record = dyn_cast<CXXRecordDecl>(field->getParent());
        if (!record) return {};
        identity += ":union=" + recordIdentity(record, ctx) + ":member=" + std::to_string(field->getFieldIndex());
      }
      if (member->isArrow()) break;
      expr = member->getBase();
    }
    out.set_type(identity);
    return out;
  }
  MValue offset(MValue base, uint64_t bytes) {
    MValue out; out.set_kind(MValue::MV_OFFSET); out.set_offset(bytes); *out.mutable_base() = std::move(base); return out;
  }
  MValue address(const Expr *expr, unsigned depth = 0) {
    MValue out;
    if (!expr || depth > 64 || expr->isInstantiationDependent()) return out;
    expr = expr->IgnoreParenImpCasts();
    if (const auto *cast=dyn_cast<CXXStaticCastExpr>(expr))
      if (cast->getCastKind()==CK_NoOp && cast->isGLValue() &&
          ctx.hasSameType(cast->getType(),cast->getSubExpr()->getType()))
        return address(cast->getSubExpr(),depth+1);
    if (const auto *call = dyn_cast<CallExpr>(expr)) {
      // Clang retains the reference result's value category even when the
      // target is indirect. The call operation must still establish the
      // actual returned address; this never invents a referent.
      if (call->isLValue() && storageType(call->getType())) {
        auto found = calls.find(call);
        if (found != calls.end()) { out.set_kind(MValue::MV_TEMPORARY); out.set_index(found->second); }
      }
      return out;
    }
    if (const auto *ref = dyn_cast<DeclRefExpr>(expr)) {
      const auto *var = dyn_cast<VarDecl>(ref->getDecl());
      if (const auto *parameter = dyn_cast_or_null<ParmVarDecl>(var)) {
        if (parameter->getType()->isLValueReferenceType() && storageType(parameter->getType()->getPointeeType())) {
          out.set_kind(MValue::MV_ARGUMENT); out.set_index(parameter->getFunctionScopeIndex() + (receiver ? 1 : 0)); return out;
        }
      }
      auto binding=references.find(var);
      if(binding!=references.end()) {out.set_kind(MValue::MV_TEMPORARY);out.set_index(binding->second);return out;}
      auto found = locals.find(var);
      if (found != locals.end()) { out.set_kind(MValue::MV_LOCAL); out.set_index(found->second); }
    } else if (const auto *member = dyn_cast<MemberExpr>(expr)) {
      const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
      if (!field || field->isBitField() || field->getType()->isReferenceType() || !field->getParent()->isCompleteDefinition()) return out;
      const auto bits = ctx.getASTRecordLayout(field->getParent()).getFieldOffset(field->getFieldIndex());
      if (bits % ctx.getCharWidth()) return out;
      return offset(member->isArrow() ? value(member->getBase(), depth + 1) : address(member->getBase(), depth + 1), bits / ctx.getCharWidth());
    } else if (const auto *unary = dyn_cast<UnaryOperator>(expr)) {
      if (unary->getOpcode() == UO_Deref) return value(unary->getSubExpr(), depth + 1);
    } else if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(expr)) {
      Expr::EvalResult index;
      if (!storageType(subscript->getType()) || !subscript->getIdx()->EvaluateAsInt(index, ctx) || index.Val.getInt().isNegative() || index.Val.getInt().getActiveBits() > 32) return out;
      const auto bytes = ctx.getTypeSizeInChars(subscript->getType()).getQuantity();
      return offset(value(subscript->getBase(), depth + 1), index.Val.getInt().getZExtValue() * bytes);
    }
    return out;
  }
  MValue load(const Expr *expr, unsigned depth) {
    MValue out;
    auto type = access(expr->getType(), expr);
    if (!type.bytes()) return out;
    out.set_kind(MValue::MV_LOAD); *out.mutable_access() = std::move(type); *out.mutable_base() = address(expr, depth + 1); return out;
  }
  MValue value(const Expr *expr, unsigned depth = 0) {
    MValue out;
    if (!expr || depth > 64 || expr->isInstantiationDependent() || expr->getType().isNull()) return out;
    expr = expr->IgnoreParens();
    if (isa<CXXThisExpr>(expr) && receiver) {out.set_kind(MValue::MV_ARGUMENT);out.set_index(0);return out;}
    if (const auto *call = dyn_cast<CallExpr>(expr)) {
      auto found = calls.find(call); if (found != calls.end()) {out.set_kind(MValue::MV_TEMPORARY);out.set_index(found->second);} return out;
    }
    // Enumerators are values, not loads from an object with unknown address.
    // Retain their exact value when representable by the memory IR integer.
    if (const auto *reference = dyn_cast<DeclRefExpr>(expr))
      if (const auto *enumerator = dyn_cast<EnumConstantDecl>(reference->getDecl())) {
        const auto &integer = enumerator->getInitVal();
        if (integer.isSignedIntN(64) && (!integer.isUnsigned() || integer.getActiveBits()<64)) {
          out.set_kind(MValue::MV_INTEGER); out.set_integer(integer.isUnsigned() ? static_cast<int64_t>(integer.getZExtValue()) : integer.getSExtValue());
        }
        return out;
      }
    // Function values retain exact identity separately from storage epochs.
    if (const auto *reference = dyn_cast<DeclRefExpr>(expr))
      if (const auto *function=dyn_cast<FunctionDecl>(reference->getDecl())) {
        out.set_kind(MValue::MV_FUNCTION);
        computeFuncEntityId(const_cast<FunctionDecl *>(function),ctx,ctx.getSourceManager(),out.mutable_function());
        return out;
      }
    if (const auto *cast = dyn_cast<CastExpr>(expr)) {
      switch (cast->getCastKind()) {
        case CK_FunctionToPointerDecay: return value(cast->getSubExpr(), depth + 1);
        case CK_LValueToRValue: return load(cast->getSubExpr(), depth + 1);
        case CK_ArrayToPointerDecay: return address(cast->getSubExpr(), depth + 1);
        case CK_NullToPointer: out.set_kind(MValue::MV_NULL); return out;
        case CK_NoOp:
          // Explicit numeric casts can wrap the real implicit conversion in
          // a same-type NoOp node. Preserve that inner conversion and value.
          if ((cast->getType()->isArithmeticType() || cast->getType()->isEnumeralType()) &&
              ctx.hasSameUnqualifiedType(cast->getType(),cast->getSubExpr()->getType()))
            return value(cast->getSubExpr(), depth + 1);
          [[fallthrough]];
        case CK_BitCast:
          if (cast->getType()->isFunctionPointerType() || cast->getSubExpr()->getType()->isFunctionPointerType()) {
            if (!ctx.hasSameType(cast->getType(),cast->getSubExpr()->getType())) return out;
          }
          if (cast->getType()->isPointerType() && cast->getSubExpr()->getType()->isPointerType()) return value(cast->getSubExpr(), depth + 1);
          return out;
        case CK_IntegralToBoolean:
          // Reuse comparison transfer for contextual integer truth values.
          // sizeof(bool) is not the value range of a numeric narrowing cast.
          out.set_kind(MValue::MV_BINARY);out.set_binary_operator("!=");integerType(out,cast->getType());
          *out.mutable_base()=value(cast->getSubExpr(),depth+1);
          out.mutable_rhs()->set_kind(MValue::MV_INTEGER);out.mutable_rhs()->set_integer(0);return out;
        case CK_IntegralCast:
          out.set_kind(MValue::MV_INTEGER_CAST); integerType(out, cast->getType()); *out.mutable_base() = value(cast->getSubExpr(), depth + 1); return out;
        default: return out;
      }
    }
    if (isa<DeclRefExpr, MemberExpr, ArraySubscriptExpr>(expr)) return load(expr, depth + 1);
    if (const auto *unary = dyn_cast<UnaryOperator>(expr)) {
      if (unary->getOpcode() == UO_AddrOf)
        if (const auto *reference = dyn_cast<DeclRefExpr>(unary->getSubExpr()->IgnoreParenImpCasts()))
          if (const auto *function=dyn_cast<FunctionDecl>(reference->getDecl())) {
        out.set_kind(MValue::MV_FUNCTION);
        computeFuncEntityId(const_cast<FunctionDecl *>(function),ctx,ctx.getSourceManager(),out.mutable_function());
        return out;
      }
      if (unary->getOpcode() == UO_AddrOf) return address(unary->getSubExpr(), depth + 1);
      if (unary->getOpcode() == UO_Deref && unary->getType()->isFunctionType()) return value(unary->getSubExpr(), depth + 1);
      if (unary->getOpcode() == UO_Deref) return load(expr, depth + 1);
    }
    if (isa<CXXNullPtrLiteralExpr>(expr)) {out.set_kind(MValue::MV_NULL);return out;}
    if (isa<FloatingLiteral>(expr)) {out.set_kind(MValue::MV_SCALAR);return out;}
    Expr::EvalResult constant;
    if (!expr->HasSideEffects(ctx) && expr->getType()->isIntegerType() && expr->EvaluateAsInt(constant, ctx) && constant.Val.getInt().isSignedIntN(64) && (!constant.Val.getInt().isUnsigned() || constant.Val.getInt().getActiveBits() < 64)) {
      out.set_kind(MValue::MV_INTEGER);out.set_integer(constant.Val.getInt().isUnsigned() ? static_cast<int64_t>(constant.Val.getInt().getZExtValue()) : constant.Val.getInt().getSExtValue());return out;
    }
    if (const auto *binary = dyn_cast<BinaryOperator>(expr)) {
      std::vector<facts_pb::EntityId> requirements;
      std::vector<uint32_t> evaluatedCalls;
      if (stableArgument(binary,0,&requirements,&evaluatedCalls) && (binary->getOpcode() == BO_Add || binary->getOpcode() == BO_Sub || binary->getOpcode() == BO_Mul || binary->getOpcode() == BO_Div || binary->getOpcode() == BO_Rem || binary->isShiftOp() || binary->isComparisonOp()) && binary->getType()->isIntegerType()) {
        for (const auto &id:requirements) *out.add_evaluation_callees()=id;
        for (const auto id:evaluatedCalls) out.add_evaluated_call_temporaries(id);
        out.set_kind(MValue::MV_BINARY);out.set_binary_operator(binary->getOpcodeStr().str());integerType(out,binary->getType());
        *out.mutable_base() = value(binary->getLHS(), depth + 1); *out.mutable_rhs() = value(binary->getRHS(), depth + 1);
      }
    }
    return out;
  }
  void store(facts_pb::MemoryBlock *block, MValue destination, facts_pb::MemoryAccess type, MValue value) {
    auto *op = block->add_operations();
    if (!type.bytes()) {op->set_kind(MOp::MO_HAVOC);return;}
    op->set_kind(MOp::MO_STORE);*op->mutable_destination() = std::move(destination);*op->mutable_access() = std::move(type);*op->mutable_value() = std::move(value);
  }
  bool pureAggregateInitialization(const Stmt *stmt, unsigned depth = 0) const {
    if (!stmt || depth > 64 || isa<CallExpr,CXXConstructExpr>(stmt)) return false;
    if (const auto *expr=dyn_cast<Expr>(stmt)) if(expr->HasSideEffects(ctx)) return false;
    for(const auto *child:stmt->children()) if(child && !pureAggregateInitialization(child,depth+1)) return false;
    return true;
  }
  // Compiler-trivial copies use the same typed loads/stores as ordinary
  // expressions. Validate the entire layout before emitting any transfer.
  bool copyLeaves(QualType type, uint64_t position,
                  std::vector<std::pair<uint64_t, QualType>> &leaves, unsigned depth = 0) {
    if (++initializationNodes > 8192) {complete=false;return false;}
    if (depth > 8 || !storageType(type) || type.isVolatileQualified()) return false;
    if (type->isScalarType()) {
      if (!type->isIntegerType() && !type->isEnumeralType() && !type->isPointerType()) return false;
      leaves.emplace_back(position,type);return true;
    }
    if (const auto *array=ctx.getAsConstantArrayType(type)) {
      const uint64_t count=array->getSize().getLimitedValue(513);
      const auto bytes=ctx.getTypeSizeInChars(array->getElementType()).getQuantity();
      if (count>512 || bytes<=0 || (count && uint64_t(bytes)>(std::numeric_limits<uint64_t>::max()-position)/count)) return false;
      for(uint64_t i=0;i<count;++i)
        if(!copyLeaves(array->getElementType(),position+i*uint64_t(bytes),leaves,depth+1)) return false;
      return true;
    }
    const auto *record=type->getAsCXXRecordDecl();
    if (!record || record->isUnion() || record->getNumBases()) return false;
    for(const auto *field:record->fields()) {
      if(field->isBitField()) return false;
      const uint64_t bits=ctx.getASTRecordLayout(record).getFieldOffset(field->getFieldIndex());
      if(bits%ctx.getCharWidth() || bits/ctx.getCharWidth()>std::numeric_limits<uint64_t>::max()-position ||
         !copyLeaves(field->getType(),position+bits/ctx.getCharWidth(),leaves,depth+1)) return false;
    }
    return true;
  }
  // Snapshot both addresses before touching fields: aliases may be stored
  // inside the destination itself. Typed leaf loads consume source demand.
  MValue copyTransfer(facts_pb::MemoryBlock *block, MValue destination, MValue source,
                     const std::vector<std::pair<uint64_t,QualType>> &leaves,
                     const std::vector<facts_pb::EntityId> &requirements = {},
                     const std::vector<uint32_t> &evaluatedCalls = {}) {
    MValue addresses[2];
    for(unsigned i=0;i<2;++i) {
      auto *binding=block->add_operations();binding->set_kind(MOp::MO_BIND);
      binding->set_temporary(copyTemporary++);
      *binding->mutable_value()=i==0 ? source : destination;
      if(i==0) {
        for(const auto &id:requirements) *binding->add_evaluation_callees()=id;
        for(const auto id:evaluatedCalls) binding->add_evaluated_call_temporaries(id);
      }
      addresses[i].set_kind(MValue::MV_TEMPORARY);addresses[i].set_index(binding->temporary());
    }
    for(const auto &leaf:leaves) {
      MValue loaded;loaded.set_kind(MValue::MV_LOAD);
      *loaded.mutable_base()=offset(addresses[0],leaf.first);
      *loaded.mutable_access()=access(leaf.second);integerType(loaded,leaf.second);
      store(block,offset(addresses[1],leaf.first),access(leaf.second),std::move(loaded));
    }
    return addresses[1];
  }
  bool copyAssignment(facts_pb::MemoryBlock *block, const CXXOperatorCallExpr *call) {
    const auto *method=dyn_cast_or_null<CXXMethodDecl>(call->getDirectCallee());
    if (!method || !method->isTrivial() || call->getNumArgs()!=2 ||
        (!method->isCopyAssignmentOperator() && !method->isMoveAssignmentOperator())) return false;
    const auto *destination=call->getArg(0), *source=call->getArg(1);
    if (!ctx.hasSameUnqualifiedType(destination->getType(),source->getType()) ||
        destination->getType().isVolatileQualified() || source->getType().isVolatileQualified()) return false;
    std::vector<facts_pb::EntityId> requirements;
    std::vector<uint32_t> evaluatedCalls;
    std::vector<std::pair<uint64_t,QualType>> leaves;
    if (!stableArgument(source,0,&requirements,&evaluatedCalls) ||
        !stableArgument(destination,0,&requirements,&evaluatedCalls) ||
        !copyLeaves(destination->getType(),0,leaves)) return false;
    const auto result=copyTransfer(block,address(destination),address(source),leaves,requirements,evaluatedCalls);
    auto *binding=block->add_operations();binding->set_kind(MOp::MO_BIND);
    binding->set_temporary(calls.at(call));*binding->mutable_value()=result;
    return true;
  }
  void initialize(facts_pb::MemoryBlock *block, MValue destination, QualType type, const Expr *expr, unsigned depth = 0, bool qualify = false) {
    if (!expr) return;
    if (++initializationNodes > 8192) {complete=false;block->add_operations()->set_kind(MOp::MO_HAVOC);return;}
    if (depth > 8) {block->add_operations()->set_kind(MOp::MO_HAVOC);return;}
    if (depth == 0) qualify = pureAggregateInitialization(expr);
    if (type.isVolatileQualified()) qualify = false;
    if (type->isScalarType()) {
      MValue initialized=value(expr);
      if (isa<ImplicitValueInitExpr>(expr)) {
        // Language value initialization is typed zero/null, not a raw-byte
        // memset contract. These implicit values have no source obligation.
        initialized.Clear();
        if (type->isPointerType()) initialized.set_kind(MValue::MV_NULL);
        else if (type->isIntegerType() || type->isEnumeralType()) {initialized.set_kind(MValue::MV_INTEGER);initialized.set_integer(0);}
        else initialized.set_kind(MValue::MV_SCALAR);
      }
      store(block, std::move(destination), access(type), std::move(initialized));
      const auto ordinal=expressionOrdinals.find(expr);
      if (depth > 0 && qualify && ordinal!=expressionOrdinals.end() &&
          (type->isIntegerType() || type->isPointerType()) && !type.isVolatileQualified() &&
          !isa<ImplicitValueInitExpr,CXXDefaultInitExpr>(expr) && expr->getExprLoc().isValid() &&
          !isSystemLoc(expr->getExprLoc(),ctx.getSourceManager())) {
        auto *op=block->mutable_operations(block->operations_size()-1);
        if (op->kind()==MOp::MO_STORE) {
          // Bit59 separates initializer AST ordinals from assignment ordinals.
          const uint64_t id=(uint64_t(5)<<60)|(uint64_t(1)<<59)|ordinal->second;
          op->set_obligation_id(id);setLoc(op->mutable_loc(),expr->getExprLoc(),ctx.getSourceManager());
          facts_pb::Fact fact;fact.set_origin(originOf(false));auto *candidate=fact.mutable_baseline_candidate();
          candidate->set_rule_id("0.1.1");candidate->set_reason("aggregate-initializer-needs-absent-read-demand-proof");
          *candidate->mutable_loc()=op->loc();*candidate->mutable_initialization_function()=function;
          candidate->set_initialization_obligation_id(id);g_facts.push_back(std::move(fact));
        }
      }
      return;
    }
    const auto *list = dyn_cast<InitListExpr>(expr->IgnoreParenImpCasts());
    const bool implicit = isa<ImplicitValueInitExpr>(expr);
    if (const auto *array=ctx.getAsConstantArrayType(type)) {
      const uint64_t count=array->getSize().getLimitedValue(513);
      if (count>512 || (!implicit && (!list || !list->isSemanticForm())) || (list && list->getNumInits()>count)) {
        block->add_operations()->set_kind(MOp::MO_HAVOC);return;
      }
      const auto elementType=array->getElementType();
      const auto bytes=ctx.getTypeSizeInChars(elementType).getQuantity();
      if (bytes<=0 || (count && uint64_t(bytes)>std::numeric_limits<uint64_t>::max()/count)) {block->add_operations()->set_kind(MOp::MO_HAVOC);return;}
      for(uint64_t i=0;i<count;++i) {
        const Expr *element=implicit ? expr : (i<list->getNumInits() ? list->getInit(i) : list->getArrayFiller());
        if (!element) {block->add_operations()->set_kind(MOp::MO_HAVOC);return;}
        initialize(block,offset(destination,i*uint64_t(bytes)),elementType,element,depth+1,qualify);
        if (!complete) return;
      }
      return;
    }
    const auto *record = type->getAsCXXRecordDecl();
    if ((implicit || (list && list->isSemanticForm())) && record && !record->isUnion()) {
      unsigned i = 0;
      for (const auto *field : record->fields()) {
        if (field->isBitField() || (!implicit && i >= list->getNumInits())) {block->add_operations()->set_kind(MOp::MO_HAVOC);return;}
        const auto bits = ctx.getASTRecordLayout(record).getFieldOffset(field->getFieldIndex());
        if (bits % ctx.getCharWidth()) {block->add_operations()->set_kind(MOp::MO_HAVOC);return;}
        const Expr *element=implicit ? expr : list->getInit(i++);
        initialize(block, offset(destination, bits / ctx.getCharWidth()), field->getType(), element, depth + 1, qualify);
        if (!complete) return;
      }
      return;
    }
    const auto *construct = dyn_cast<CXXConstructExpr>(expr->IgnoreParenImpCasts());
    if (construct && construct->getConstructor()->isTrivial() && construct->getConstructor()->isDefaultConstructor()) {
      if (construct->requiresZeroInitialization()) {
        // Value-initialization zeroes the object before its trivial default
        // constructor. Reuse typed aggregate initialization (including null
        // pointers); default-initialization alone still leaves storage unset.
        ImplicitValueInitExpr zero(type);
        initialize(block, std::move(destination), type, &zero, depth + 1, false);
      }
      return;
    }
    if (depth==0 && construct && construct->getConstructor()->isTrivial() &&
        (construct->getConstructor()->isCopyConstructor() || construct->getConstructor()->isMoveConstructor()) &&
        construct->getNumArgs()==1 && destination.kind()==MValue::MV_LOCAL) {
      const auto *source=construct->getArg(0);
      std::vector<std::pair<uint64_t,QualType>> leaves;
      std::vector<facts_pb::EntityId> requirements;
      std::vector<uint32_t> evaluatedCalls;
      if(!source->getType().isVolatileQualified() && ctx.hasSameUnqualifiedType(type,source->getType()) &&
         stableArgument(source,0,&requirements,&evaluatedCalls) && copyLeaves(type,0,leaves)) {
        copyTransfer(block,destination,address(source),leaves,requirements,evaluatedCalls);
        return;
      }
    }
    block->add_operations()->set_kind(MOp::MO_HAVOC);
  }
};
// A complete byte-memory CFG is not a language-definedness certificate. Keep
// this contract explicit and exhaustive: unsupported syntax cannot silently
// disappear from a normal-return proof. Callee bodies are checked transitively
// by the evaluator; this frontend checks their signatures and local syntax.
class NormalReturnModel : public RecursiveASTVisitor<NormalReturnModel> {
  ASTContext &ctx;
 public:
  bool valid = true;
  unsigned nodes = 0;
  explicit NormalReturnModel(ASTContext &context) : ctx(context) {}
  bool integer(QualType type) const {
    return !type.isNull() && !type.isVolatileQualified() && type->isIntegerType() &&
      !type->isEnumeralType() && !type->isDependentType() && ctx.getTypeSize(type)<=64;
  }
  bool signature(const FunctionDecl *fn) const {
    if (!fn || isa<CXXMethodDecl>(fn) || fn->isVariadic() || fn->isDependentContext() ||
        fn->hasAttr<ReturnsTwiceAttr>() || fn->getBuiltinID() ||
        !(fn->getReturnType()->isVoidType() || integer(fn->getReturnType()))) return false;
    for (const auto *param:fn->parameters()) if (!integer(param->getType())) return false;
    return true;
  }
  bool VisitStmt(Stmt *stmt) {
    if (++nodes>8192) {valid=false;return false;}
    if (!isa<CompoundStmt,DeclStmt,ReturnStmt,IfStmt,NullStmt,ParenExpr,
        IntegerLiteral,CharacterLiteral,CXXBoolLiteralExpr,DeclRefExpr,
        CastExpr,UnaryOperator,BinaryOperator,CallExpr>(stmt)) valid=false;
    return true;
  }
  bool VisitVarDecl(VarDecl *decl) {
    if (!decl->hasLocalStorage() || decl->hasAttr<CleanupAttr>() || !integer(decl->getType())) valid=false;
    return true;
  }
  bool VisitDeclRefExpr(DeclRefExpr *expr) {
    if (const auto *decl=dyn_cast<VarDecl>(expr->getDecl())) {
      if (!decl->hasLocalStorage() || !integer(decl->getType())) valid=false;
    } else if (const auto *fn=dyn_cast<FunctionDecl>(expr->getDecl())) {
      if (!signature(fn)) valid=false;
    } else valid=false;
    return true;
  }
  bool VisitCastExpr(CastExpr *expr) {
    const auto kind=expr->getCastKind();
    if (kind==CK_FunctionToPointerDecay) return true; // only direct calls can consume it
    if ((kind!=CK_LValueToRValue && kind!=CK_IntegralCast && kind!=CK_IntegralToBoolean && kind!=CK_NoOp) ||
        !integer(expr->getType()) || !integer(expr->getSubExpr()->getType())) valid=false;
    return true;
  }
  bool VisitUnaryOperator(UnaryOperator *expr) {
    const auto op=expr->getOpcode();
    if (!integer(expr->getType()) || (op!=UO_Plus && op!=UO_LNot &&
        !(op==UO_Minus && isa<IntegerLiteral>(expr->getSubExpr()->IgnoreParenImpCasts())))) valid=false;
    return true;
  }
  bool VisitBinaryOperator(BinaryOperator *expr) {
    if (!integer(expr->getType()) || expr->isCompoundAssignmentOp()) valid=false;
    if (expr->isAssignmentOp()) {
      const auto parents=ctx.getParents(*expr);
      if (parents.size()!=1 || !parents[0].get<CompoundStmt>() ||
          !isa<DeclRefExpr>(expr->getLHS()->IgnoreParens())) valid=false;
    }
    return true;
  }
  bool VisitCallExpr(CallExpr *expr) {
    const auto *callee=expr->getDirectCallee();
    // Calling a noreturn function which returns is itself undefined. It cannot
    // establish defined continuation of a surrounding noreturn function.
    if (!signature(callee) || callee->isNoReturn() || expr->getNumArgs()!=callee->getNumParams()) valid=false;
    return true;
  }
};
void buildMemoryFacts(FunctionDecl *fn, ASTContext &ctx, facts_pb::InitializationFunction *parent,
                      const InitializationCallSites &callSites,
                      const InitializationAccessSites &accessSites) {
  using MV = facts_pb::MemoryValue; using MO = facts_pb::MemoryOperation;
  // Reuse the existing exact nonvirtual getter receiver contract. Its implicit
  // this is memory argument zero; declared scalar parameters follow it.
  const bool receiver=isa<CXXMethodDecl>(fn) && parent->has_record_receiver() &&
      !parent->receiver_is_parameter() && !parent->is_constructor();
  auto *out = parent->mutable_memory_flow();out->set_version(5);out->set_parameter_count(fn->getNumParams()+(receiver?1:0));
  if ((isa<CXXMethodDecl>(fn) && !receiver) || fn->isVariadic() || fn->isDependentContext() ||
      fn->getReturnType()->isRValueReferenceType() || out->parameter_count() > 128) return;
  const auto &language = ctx.getLangOpts();
  out->set_cxx_standard(language.CPlusPlus26 ? 26 : language.CPlusPlus23 ? 23 :
      language.CPlusPlus20 ? 20 : language.CPlusPlus17 ? 17 :
      language.CPlusPlus14 ? 14 : language.CPlusPlus11 ? 11 :
      language.CPlusPlus ? 98 : 0);
  NormalReturnModel returnModel(ctx);
  returnModel.valid=returnModel.signature(fn);
  for (auto *param:fn->parameters()) returnModel.VisitVarDecl(param);
  returnModel.TraverseStmt(fn->getBody());
  MemoryFacts flow(ctx,receiver);flow.function=parent->function();
  if (fn->getReturnType()->isLValueReferenceType() &&
      !flow.storageType(fn->getReturnType()->getPointeeType())) return;
  for (auto *param : fn->parameters()) flow.VisitVarDecl(param);
  flow.TraverseStmt(fn->getBody());out->set_local_count(flow.locals.size());
  if (!flow.complete) return;
  CFG::BuildOptions options;options.setAllAlwaysAdd();options.AddLifetime=true;
  auto cfg = CFG::buildCFG(fn,fn->getBody(),&ctx,options);
  if (!cfg || cfg->size() > 512) return;
  const auto bindAccess=[&](facts_pb::MemoryOperation *operation,const Expr *expression) {
    if (!expression) return;
    const auto found=accessSites.find(std::make_pair(expression->IgnoreParenImpCasts(),unsigned(operation->kind()==facts_pb::MemoryOperation::MO_STORE)));
    if (found==accessSites.end() || !found->second.first || !found->second.second) return;
    operation->mutable_access_site()->set_full_expr_id(found->second.first);
    operation->mutable_access_site()->set_node_id(found->second.second);
  };
  for (const auto *block : *cfg) {
    auto *target = out->add_blocks();target->set_block_id(block->getBlockID());target->set_is_entry(block == &cfg->getEntry());target->set_no_return(block->hasNoReturnElement());
    for (auto edge=block->succ_begin();edge!=block->succ_end();++edge) if (const auto *next=edge->getReachableBlock()) target->add_successors(next->getBlockID());
    // Clang orders if/logical successors by true/false (for ||, RHS is
    // the false edge). Logical terminator conditions are the LHS expression.
    // Preserve refinement only when both successor identities are present.
    const auto *logical = dyn_cast_or_null<BinaryOperator>(block->getTerminatorStmt());
    if ((isa_and_nonnull<IfStmt>(block->getTerminatorStmt()) || (logical && logical->isLogicalOp())) && block->succ_size()==2 && target->successors_size()==2)
      if (const auto *condition=dyn_cast_or_null<Expr>(block->getTerminatorCondition()))
        *target->mutable_condition()=flow.value(condition);
    auto begin = [&](const VarDecl *v) {
      auto found=flow.locals.find(v);if(found==flow.locals.end()) return MV{};
      auto *op=target->add_operations();op->set_kind(MO::MO_BEGIN_LOCAL);op->set_temporary(found->second);
      op->mutable_value()->set_kind(MV::MV_INTEGER);op->mutable_value()->set_integer(ctx.getTypeSizeInChars(v->getType()).getQuantity());
      MV result;result.set_kind(MV::MV_LOCAL);result.set_index(found->second);return result;
    };
    if (block==&cfg->getEntry()) for (unsigned i=0;i<fn->getNumParams();++i) {
      if (fn->getParamDecl(i)->getType()->isLValueReferenceType()) continue;
      auto address=begin(fn->getParamDecl(i));MV input;input.set_kind(MV::MV_ARGUMENT);input.set_index(i+(receiver?1:0));
      flow.store(target,std::move(address),flow.access(fn->getParamDecl(i)->getType()),std::move(input));
    }
    for (const auto &element : *block) {
      if (auto end=element.getAs<CFGLifetimeEnds>()) {
        auto found=flow.locals.find(end->getVarDecl());
        if(found!=flow.locals.end()) {
          auto *op=target->add_operations();op->set_kind(MO::MO_END_LOCAL);
          op->mutable_value()->set_kind(MV::MV_LOCAL);op->mutable_value()->set_index(found->second);
        }
        continue;
      }
      auto item=element.getAs<CFGStmt>();if(!item) {flow.complete=false;continue;}
      const auto *stmt=item->getStmt();
      if (const auto *expr=dyn_cast<Expr>(stmt)) if(flow.reads.count(expr)) {
        auto *op=target->add_operations();op->set_kind(MO::MO_READ);
        bindAccess(op,expr);
        // Version 4 binds only the exact scalar-flow AST observation. Source
        // lines and memory-CFG traversal counters are not obligation identities.
        const auto anchor = anchors.find(expr);
        if (anchor != anchors.end() &&
            anchor->second.first.id() == parent->function().id() &&
            anchor->second.first.id_hi() == parent->function().id_hi())
          op->set_obligation_id(anchor->second.second);
        *op->mutable_value()=flow.value(expr);setLoc(op->mutable_loc(),expr->getExprLoc(),ctx.getSourceManager());
        if (op->value().kind()==MV::MV_LOAD && !isSystemLoc(expr->getExprLoc(),ctx.getSourceManager())) {
          // Scalar extraction can reject a function that this memory model
          // supports. Give its read an independent, positive-only observation.
          if (!op->obligation_id()) {
            const uint64_t obligation=(uint64_t(1)<<59)|flow.reads.at(expr);
            op->set_obligation_id(obligation);
            facts_pb::Fact fact;fact.set_origin(originOf(false));
            auto *candidate=fact.mutable_baseline_candidate();
            candidate->set_rule_id("11.6.2");candidate->set_reason("memory-read-needs-uninitialized-proof");
            *candidate->mutable_loc()=op->loc();*candidate->mutable_initialization_function()=parent->function();
            candidate->set_initialization_obligation_id(obligation);g_facts.push_back(std::move(fact));
          }
          // Stable AST ordinal, independent of memory-CFG traversal order.
          const uint64_t obligation=(uint64_t(3)<<60)|flow.reads.at(expr);
          auto value=op->value(); auto loc=op->loc();
          auto *lifetime=target->add_operations();lifetime->set_kind(MO::MO_READ);
          lifetime->set_obligation_id(obligation);*lifetime->mutable_value()=std::move(value);*lifetime->mutable_loc()=loc;
          facts_pb::Fact fact;fact.set_origin(originOf(false));auto *candidate=fact.mutable_baseline_candidate();
          candidate->set_rule_id("6.8.1");candidate->set_reason("value-read-needs-ended-storage-proof");
          *candidate->mutable_loc()=loc;*candidate->mutable_initialization_function()=parent->function();
          candidate->set_initialization_obligation_id(obligation);g_facts.push_back(std::move(fact));
        }
      }
      if (const auto *decls=dyn_cast<DeclStmt>(stmt)) {
        for (const auto *decl:decls->decls()) if(const auto *v=dyn_cast<VarDecl>(decl)) {
          const auto binding=flow.references.find(v);
          if(binding!=flow.references.end()) {
            auto value=flow.address(v->getInit());
            if(value.kind()==MV::MV_UNKNOWN || value.kind()==MV::MV_NULL) {flow.complete=false;continue;}
            auto *op=target->add_operations();op->set_kind(MO::MO_BIND);op->set_temporary(binding->second);*op->mutable_value()=std::move(value);
          } else flow.initialize(target,begin(v),v->getType(),v->getInit());
        }
      } else if (const auto *binary=dyn_cast<BinaryOperator>(stmt)) {
        const auto arithmetic=flow.arithmetic.find(binary);
        if (arithmetic!=flow.arithmetic.end() && !isSystemLoc(binary->getOperatorLoc(),ctx.getSourceManager())) {
          auto value=flow.value(binary);
          if (value.kind()==MV::MV_BINARY) {
            const bool definedOperation=binary->getOpcode()==BO_Div || binary->getOpcode()==BO_Rem || binary->isShiftOp();
            const uint64_t obligation=(uint64_t(1)<<60)|(definedOperation ? uint64_t(1)<<59 : 0)|arithmetic->second;
            auto *op=target->add_operations();op->set_kind(MO::MO_READ);op->set_obligation_id(obligation);
            *op->mutable_value()=std::move(value);setLoc(op->mutable_loc(),binary->getOperatorLoc(),ctx.getSourceManager());
            facts_pb::Fact fact;fact.set_origin(originOf(false));auto *candidate=fact.mutable_baseline_candidate();
            candidate->set_rule_id("4.1.3");candidate->set_reason(definedOperation ? "arithmetic-needs-defined-operation-proof" : "arithmetic-needs-overflow-proof");
            *candidate->mutable_loc()=op->loc();*candidate->mutable_initialization_function()=parent->function();
            candidate->set_initialization_obligation_id(obligation);g_facts.push_back(std::move(fact));
          }
        }
        if (binary->isAssignmentOp()) {
          flow.store(target,flow.address(binary->getLHS()),flow.access(binary->getLHS()->getType(),binary->getLHS()),binary->getOpcode()==BO_Assign ? flow.value(binary->getRHS()) : MV{});
          const auto anchor=flow.deadStores.find(binary);
          auto *op=target->mutable_operations(target->operations_size()-1);
          if (op->kind()==MO::MO_STORE) bindAccess(op,binary->getLHS());
          if (anchor!=flow.deadStores.end() && op->kind()==MO::MO_STORE && !isSystemLoc(binary->getOperatorLoc(),ctx.getSourceManager())) {
            const uint64_t obligation=(uint64_t(5)<<60)|anchor->second;
            op->set_obligation_id(obligation);setLoc(op->mutable_loc(),binary->getOperatorLoc(),ctx.getSourceManager());
            facts_pb::Fact fact;fact.set_origin(originOf(false));auto *candidate=fact.mutable_baseline_candidate();
            candidate->set_rule_id("0.1.1");candidate->set_reason("store-needs-absent-read-demand-proof");
            *candidate->mutable_loc()=op->loc();*candidate->mutable_initialization_function()=parent->function();
            candidate->set_initialization_obligation_id(obligation);g_facts.push_back(std::move(fact));
          }
        }
      } else if (const auto *unary=dyn_cast<UnaryOperator>(stmt)) {
        if (unary->isIncrementDecrementOp()) flow.store(target,flow.address(unary->getSubExpr()),flow.access(unary->getSubExpr()->getType(),unary->getSubExpr()),MV{});
      } else if (const auto *call=dyn_cast<CallExpr>(stmt)) {
        if (const auto *assignment=dyn_cast<CXXOperatorCallExpr>(call))
          if (flow.copyAssignment(target,assignment)) continue;
        auto *op=target->add_operations();op->set_temporary(flow.calls[call]);setLoc(op->mutable_loc(),call->getExprLoc(),ctx.getSourceManager());
        // Keep exact identity even when unsupported binding leaves this call
        // as havoc. Consumers must not interpret the link as a resolution.
        const auto site=callSites.find(call);
        if (site!=callSites.end()) {
          op->mutable_call_site()->set_full_expr_id(site->second.first);
          op->mutable_call_site()->set_node_id(site->second.second);
        }
        const auto observeLogicalWrite=[&]() {
          if (flow.logicalCalls.count(call) && !isSystemLoc(call->getExprLoc(),ctx.getSourceManager())) {
            const uint64_t obligation=(uint64_t(1)<<61)|(uint64_t(flow.calls[call])+1);
            op->set_obligation_id(obligation);
            facts_pb::Fact fact;fact.set_origin(originOf(false));
            auto *candidate=fact.mutable_baseline_candidate();
            candidate->set_rule_id("8.14.1");candidate->set_reason("logical-call-needs-persistent-write-proof");
            *candidate->mutable_loc()=op->loc();*candidate->mutable_initialization_function()=parent->function();
            candidate->set_initialization_obligation_id(obligation);g_facts.push_back(std::move(fact));
          }
        };
        const auto *callee=exactDirectCallee(call);
        std::vector<facts_pb::EntityId> requirements;
        std::vector<uint32_t> evaluatedCalls;
        bool safeArguments=true;for(const auto *arg:call->arguments()) if(!flow.stableArgument(arg,0,&requirements,&evaluatedCalls)) safeArguments=false;
        if (!callee && !isa<CXXMemberCallExpr,CXXOperatorCallExpr>(call)) {
          const auto pointerType=call->getCallee()->getType();
          const auto *signature=pointerType->isFunctionPointerType()
              ? pointerType->getPointeeType()->getAs<FunctionProtoType>() : nullptr;
          if (signature && !signature->isVariadic() && signature->getNumParams()==call->getNumArgs() &&
              safeArguments && flow.stableArgument(call->getCallee(),0,&requirements,&evaluatedCalls)) {
            bool supported=true;
            for (const auto type:signature->param_types())
              if (type->isRValueReferenceType() || !flow.storageType(type->isLValueReferenceType() ? type->getPointeeType() : type)) supported=false;
            if (supported) {
              op->set_kind(MO::MO_INDIRECT_CALL);
              *op->mutable_value()=flow.value(call->getCallee());
              for(const auto &id:requirements) *op->add_evaluation_callees()=id;
        op->clear_evaluated_call_temporaries();for(const auto id:evaluatedCalls) op->add_evaluated_call_temporaries(id);
              for(unsigned i=0;i<call->getNumArgs();++i)
                *op->add_arguments()=signature->getParamType(i)->isLValueReferenceType()
                    ? flow.address(call->getArg(i)) : flow.value(call->getArg(i));
              observeLogicalWrite();
              continue;
            }
          }
        }
        if (!callee || (isa<CXXMethodDecl>(callee) && (!flow.supportedMethod(callee) || !isa<CXXMemberCallExpr>(call))) || callee->isVariadic() || call->getNumArgs()!=callee->getNumParams() || !safeArguments) {op->set_kind(MO::MO_HAVOC);continue;}
        for(const auto &id:requirements) *op->add_evaluation_callees()=id;
        op->clear_evaluated_call_temporaries();for(const auto id:evaluatedCalls) op->add_evaluated_call_temporaries(id);
        // Standard allocation fallback only. The callee identity is retained
        // so any supplied definition overrides this contract in the consumer.
        const auto name=callee->getIdentifier() ? callee->getName() : llvm::StringRef();
        const bool system=callee->isExternC() && ctx.getSourceManager().isInSystemHeader(callee->getLocation());
        computeFuncEntityId(const_cast<FunctionDecl *>(callee),ctx,ctx.getSourceManager(),op->mutable_callee());
        if (const auto *member=dyn_cast<CXXMemberCallExpr>(call)) {
          const auto *object=member->getImplicitObjectArgument();
          if (!flow.stableArgument(object,0,&requirements,&evaluatedCalls)) {op->set_kind(MO::MO_HAVOC);continue;}
          *op->add_arguments()=object->getType()->isPointerType() ? flow.value(object) : flow.address(object);
          // Receiver evaluation requirements are part of the same ordered call.
          op->clear_evaluation_callees();for(const auto &id:requirements) *op->add_evaluation_callees()=id;
        op->clear_evaluated_call_temporaries();for(const auto id:evaluatedCalls) op->add_evaluated_call_temporaries(id);
        }
        for(unsigned i=0;i<call->getNumArgs();++i)
          *op->add_arguments()=callee->getParamDecl(i)->getType()->isLValueReferenceType()
              ? flow.address(call->getArg(i)) : flow.value(call->getArg(i));
        if (system && name=="malloc" && call->getNumArgs()==1 && call->getType()->isVoidPointerType() && callee->getParamDecl(0)->getType()->isUnsignedIntegerType()) {op->set_kind(MO::MO_ALLOCATE);*op->mutable_value()=flow.value(call->getArg(0));}
        // Zero bytes only: the target certifies the null data-pointer encoding.
        // This records a storage write, not typed object initialization/lifetime.
        else if (system && name=="memset" && call->getNumArgs()==3 &&
                 callee->getReturnType()->isVoidPointerType() &&
                 callee->getParamDecl(0)->getType()->isVoidPointerType() &&
                 ctx.hasSameType(callee->getParamDecl(1)->getType(), ctx.IntTy) &&
                 callee->getParamDecl(2)->getType()->isUnsignedIntegerType() &&
                 ctx.getTargetNullPointerValue(ctx.VoidPtrTy)==0 &&
                 call->getArg(1)->getIntegerConstantExpr(ctx) &&
                 call->getArg(1)->getIntegerConstantExpr(ctx)->isZero()) {
          op->set_kind(MO::MO_ZERO_BYTES);
          *op->mutable_destination()=flow.value(call->getArg(0));
          *op->mutable_value()=flow.value(call->getArg(2));
        }
        else if (system && name=="free" && call->getNumArgs()==1 && callee->getReturnType()->isVoidType() && callee->getParamDecl(0)->getType()->isVoidPointerType()) {op->set_kind(MO::MO_FREE);*op->mutable_value()=flow.value(call->getArg(0));}
        else {
          op->set_kind(MO::MO_CALL);
          observeLogicalWrite();
        }
      } else if (const auto *returned=dyn_cast<ReturnStmt>(stmt)) {
        auto *op=target->add_operations();op->set_kind(MO::MO_RETURN);
        if (returnModel.valid && fn->isNoReturn() && !isSystemLoc(returned->getReturnLoc(),ctx.getSourceManager())) {
          const uint64_t obligation=(uint64_t(6)<<60)|(uint64_t(block->getBlockID())+1);
          op->set_obligation_id(obligation);
          setLoc(op->mutable_loc(),returned->getReturnLoc(),ctx.getSourceManager());
          facts_pb::Fact fact;fact.set_origin(originOf(false));
          auto *candidate=fact.mutable_baseline_candidate();
          candidate->set_rule_id("9.6.4");candidate->set_reason("normal-return-needs-defined-path-proof");
          setLoc(candidate->add_violation_witness_sites(),fn->getLocation(),ctx.getSourceManager());
          *candidate->mutable_loc()=op->loc();
          *candidate->mutable_initialization_function()=parent->function();
          candidate->set_initialization_obligation_id(obligation);
          g_facts.push_back(std::move(fact));
        }
        // An explicit void return evaluates no expression. Use a defined,
        // valueless marker instead of an unknown expression that could hide reads.
        if (!returned->getRetValue()) op->mutable_value()->set_kind(MV::MV_SCALAR);
        else *op->mutable_value()=fn->getReturnType()->isLValueReferenceType()
            ? flow.address(returned->getRetValue()) : flow.value(returned->getRetValue());
      } else if (const auto *construct=dyn_cast<CXXConstructExpr>(stmt)) {
        if (!construct->getConstructor()->isTrivial()) target->add_operations()->set_kind(MO::MO_HAVOC);
      }
    }
    // Observe the whole controlling value after its CFG operations, using a
    // namespace distinct from scalar reads and pointer-origin obligations.
    const auto *test = dyn_cast_or_null<IfStmt>(block->getTerminatorStmt());
    if (target->has_condition() && test && !test->isConstexpr() &&
        !isSystemLoc(test->getIfLoc(), ctx.getSourceManager())) {
      const uint64_t obligation = (uint64_t(1) << 62) | (uint64_t(block->getBlockID()) + 1);
      auto *read = target->add_operations(); read->set_kind(MO::MO_READ);
      read->set_obligation_id(obligation); *read->mutable_value() = target->condition();
      setLoc(read->mutable_loc(), test->getIfLoc(), ctx.getSourceManager());
      facts_pb::Fact fact; fact.set_origin(originOf(false));
      auto *candidate = fact.mutable_baseline_candidate();
      candidate->set_rule_id("0.0.2"); candidate->set_reason("condition-needs-value-proof");
      *candidate->mutable_loc() = read->loc();
      *candidate->mutable_initialization_function() = parent->function();
      candidate->set_initialization_obligation_id(obligation);
      g_facts.push_back(std::move(fact));
    }

  }
  out->set_complete(flow.complete);
  if (flow.complete && returnModel.valid) out->set_normal_return_model_version(1);
}

} // namespace

void collectInitializationCallers(ASTContext &ctx) {
  mathContracts.clear();
  originAnchors.clear();
  ambiguousOriginAnchors.clear();
  callerCensus.clear();
  censusOpaque = false;
  CallerVisitor(ctx).TraverseDecl(ctx.getTranslationUnitDecl());
}

void tagPointerOriginRead(const Expr *expr, facts_pb::BaselineCandidate *candidate) {
  auto found = originAnchors.find(expr);
  if (found == originAnchors.end()) return;
  *candidate->mutable_initialization_function() = found->second.first;
  candidate->set_initialization_obligation_id(found->second.second);
}

void tagInitializationRead(const Expr *expr,
                           facts_pb::BaselineCandidate *candidate) {
  const auto found = anchors.find(expr);
  if (found == anchors.end()) {
    const auto failure = readFailures.find(expr);
    candidate->set_proof_failure(failure == readFailures.end() ? "no-exact-read-anchor" : failure->second);
    return;
  }
  candidate->set_proof_failure("initialization-proof-unavailable");
  *candidate->mutable_initialization_function() = found->second.first;
  candidate->set_initialization_obligation_id(found->second.second);
}

void emitInitializationFlow(FunctionDecl *function, ASTContext &ctx,
                            const facts_pb::EntityId &id,
                            const InitializationCallSites &callSites,
                      const InitializationAccessSites &accessSites) {
  if (!function || !function->getBody())
    return;
  Flow flow(function, ctx);
  for (auto *parameter : function->parameters())
    flow.VisitVarDecl(parameter);
  if (const auto *constructor = dyn_cast<CXXConstructorDecl>(function))
    for (const auto *initializer : constructor->inits())
      flow.TraverseStmt(initializer->getInit());
  flow.TraverseStmt(function->getBody());
  if (function->isDependentContext() ||
      (isa<CXXMethodDecl>(function) && !flow.constructorRecord &&
       !flow.receiverRecord))
    flow.complete = false;
  CFG::BuildOptions options;
  options.setAllAlwaysAdd();
  options.AddInitializers = true;
  if (flow.localCount > 512 || function->getNumParams() > 128)
    flow.complete = false;
  // Unsupported functions still publish a sentinel, but do not duplicate the
  // potentially very large CFGs from system headers with unusable proof data.
  std::unique_ptr<CFG> cfg;
  if (flow.complete)
    cfg = CFG::buildCFG(function, function->getBody(), &ctx, options);
  if (cfg && cfg->size() > 512) {
    flow.complete = false;
    cfg.reset();
  }
  facts_pb::Fact fact;
  fact.set_origin(
      originOf(isSystemLoc(function->getLocation(), ctx.getSourceManager())));
  auto *out = fact.mutable_initialization_function();
  *out->mutable_function() = id;
  out->set_version(flow.recordOutput || !flow.recordOutputCalls.empty() ? 9 : 8);
  const auto census = callerCensus.find(function->getCanonicalDecl());
  if (!censusOpaque && !function->isExternallyVisible() &&
      !isa<CXXMethodDecl>(function) && !function->isVariadic() &&
      !function->hasAttrs() && !function->getTemplatedKind() &&
      census != callerCensus.end() && !census->second.escaped &&
      !census->second.callers.empty() && census->second.callers.size() <= 256) {
    // The caller inventory is complete independently of parameter types.
    // Argument proofs are per slot: unsupported pointer/object arguments stay
    // unknown without preventing a proof for unrelated scalar arguments.
    out->set_closed_callers(true);
    for (const auto &entry : census->second.callers) {
      auto *caller = out->add_callers();
      computeFuncEntityId(const_cast<FunctionDecl *>(entry.first), ctx,
                          ctx.getSourceManager(), caller->mutable_function());
      caller->set_call_count(entry.second);
    }
    std::sort(out->mutable_callers()->begin(), out->mutable_callers()->end(),
        [](const auto &a, const auto &b) {
          return std::make_pair(a.function().id(), a.function().id_hi()) <
                 std::make_pair(b.function().id(), b.function().id_hi());
        });
  }
  out->set_receiver_is_parameter(flow.receiverParameter != nullptr);
  out->set_receiver_parameter_index(
      flow.receiverParameter ? flow.receiverParameterIndex : 0);
  out->set_has_record_receiver(flow.receiverRecord != nullptr);
  if (flow.receiverRecord) {
    out->set_receiver_record_identity(recordIdentity(flow.receiverRecord, ctx));
    out->set_receiver_field_count(std::distance(
        flow.receiverRecord->field_begin(), flow.receiverRecord->field_end()));
  }
  out->set_is_constructor(flow.constructorRecord != nullptr && !flow.recordOutput);
  out->set_has_record_output_parameter(flow.recordOutput != nullptr);
  out->set_record_output_parameter_index(flow.recordOutput ? flow.recordOutputIndex : 0);
  if (flow.constructorRecord) {
    out->set_constructor_record_identity(
        recordIdentity(flow.constructorRecord, ctx));
    out->set_constructor_field_count(
        std::distance(flow.constructorRecord->field_begin(),
                      flow.constructorRecord->field_end()));
  }
  for (unsigned i = 0; i < function->getNumParams(); ++i)
    if (flow.readonlyParameters.count(function->getParamDecl(i))) out->add_readonly_parameters(i);
  out->set_has_output_parameter(flow.output != nullptr);
  out->set_output_parameter_index(flow.output ? flow.outputIndex : 0);
  out->set_own_effects_isolated(flow.ownIsolated && flow.complete);
  for (const auto &entry : flow.effectCallees)
    *out->add_effect_callees() = entry.second;
  BorrowEffects borrowed(function, ctx, out->mutable_borrowed_effects());
  out->set_definition_hash(bodyHash(function, ctx));
  out->set_parameter_count(function->getNumParams());
  bool recordParameter = false;
  for (const auto *parameter : function->parameters()) recordParameter |= supportedRecord(parameter->getType()) != nullptr;
  if (recordParameter)
    for (const auto *parameter : function->parameters()) {
      const auto *record = supportedRecord(parameter->getType());
      out->add_parameter_record_identities(record ? recordIdentity(record, ctx) : "");
    }
  out->set_local_count(flow.localCount);
  out->set_scalar_return(tracked(function->getReturnType()));
  out->set_returns_initialized_pointer(function->getReturnType()->isPointerType());
  if (const auto *record = supportedRecord(function->getReturnType()))
    out->set_return_record_identity(recordIdentity(record, ctx));
  setLoc(out->mutable_loc(), function->getLocation(), ctx.getSourceManager());
  if (!cfg)
    flow.complete = false;
  std::set<const Expr *> emitted;
  if (cfg)
    for (const auto *block : *cfg) {
      auto *target = out->add_blocks();
      target->set_block_id(block->getBlockID());
      target->set_is_entry(block == &cfg->getEntry());
      if (target->is_entry())
        for (unsigned i = 0; i < function->getNumParams(); ++i) {
          auto object = flow.objects.find(function->getParamDecl(i));
          if (object != flow.objects.end()) {
            for (auto slot : object->second.fields) {
              auto *op = target->add_operations(); op->set_kind(Operation::IO_ASSIGN); op->set_local(slot);
              op->mutable_value()->set_kind(Value::IV_PARAMETER); op->mutable_value()->set_index(i);
            }
            continue;
          }
          auto found = flow.locals.find(function->getParamDecl(i));
          if (found == flow.locals.end())
            continue;
          auto *op = target->add_operations();
          op->set_kind(Operation::IO_ASSIGN);
          op->set_local(found->second);
          op->mutable_value()->set_kind(Value::IV_PARAMETER);
          op->mutable_value()->set_index(i);
        }
      std::set<uint32_t> successors;
      for (auto it = block->succ_begin(); it != block->succ_end(); ++it)
        if (const auto *next = it->getReachableBlock())
          successors.insert(next->getBlockID());
      for (auto next : successors)
        target->add_successors(next);
      for (const auto &element : *block) {
        if (auto initialization = element.getAs<CFGInitializer>()) {
          const auto *initializer = initialization->getInitializer();
          if (!flow.constructorRecord || !initializer->isMemberInitializer()) {
            flow.complete = false;
            continue;
          }
          auto *op = target->add_operations();
          op->set_kind(Operation::IO_FIELD_ASSIGN);
          op->set_local(initializer->getMember()->getFieldIndex());
          *op->mutable_value() = flow.value(initializer->getInit());
          continue;
        }
        auto item = element.getAs<CFGStmt>();
        if (!item)
          continue;
        const auto *stmt = item->getStmt();
        auto emitRead = [&](const Expr *readExpr, const Expr *valueExpr) {
          const auto found = flow.reads.find(readExpr);
          if (found == flow.reads.end())
            return;
          if (!emitted.insert(readExpr).second) {
            flow.complete = false;
            return;
          }
          auto *op = target->add_operations();
          op->set_kind(Operation::IO_READ);
          op->set_obligation_id(found->second);
          *op->mutable_value() = flow.value(valueExpr);
          setLoc(op->mutable_loc(), readExpr->getExprLoc(), ctx.getSourceManager());
          if (!ambiguousAnchors.count(readExpr)) {
            auto previous = anchors.find(readExpr);
            if (previous != anchors.end() &&
                (previous->second.first.id() != id.id() ||
                 previous->second.first.id_hi() != id.id_hi() ||
                 previous->second.second != found->second)) {
              anchors.erase(previous);
              ambiguousAnchors.insert(readExpr);
            } else
              anchors[readExpr] = {id, found->second};
          }
        };
        if (const auto *cast = dyn_cast<ImplicitCastExpr>(stmt)) {
          emitRead(cast, cast->getSubExpr());
        } else if (const auto *construction =
                       dyn_cast<CXXConstructExpr>(stmt)) {
          emitRead(construction, construction);
          auto found = flow.constructions.find(construction);
          if (found == flow.constructions.end())
            continue;
          const auto &object = flow.objects.find(found->second)->second;
          if (construction->getConstructor()->isTrivial() && construction->getConstructor()->isDefaultConstructor()) {
            for (auto slot : object.fields) {
              auto *op = target->add_operations(); op->set_kind(Operation::IO_ASSIGN); op->set_local(slot);
              op->mutable_value()->set_kind(construction->requiresZeroInitialization() ? Value::IV_DEFINED : Value::IV_UNKNOWN);
            }
            continue;
          }
          auto copy = flow.copies.find(construction);
          if (copy != flow.copies.end()) {
            const auto &source = flow.objects.find(copy->second)->second;
            for (unsigned i = 0; i < object.fields.size(); ++i) {
              auto *op = target->add_operations();
              op->set_kind(Operation::IO_ASSIGN);
              op->set_local(object.fields[i]);
              if (!flow.poisonAll && !flow.escapedObjects.count(copy->second)) {
                op->mutable_value()->set_kind(Value::IV_LOCAL);
                op->mutable_value()->set_index(source.fields[i]);
              }
            }
            continue;
          }
          auto *op = target->add_operations();
          op->set_kind(Operation::IO_CONSTRUCT);
          computeFuncEntityId(construction->getConstructor(), ctx,
                              ctx.getSourceManager(), op->mutable_callee());
          op->set_constructor_record_identity(
              recordIdentity(object.record, ctx));
          for (auto slot : object.fields)
            op->add_destination_locals(slot);
          for (const auto *argument : construction->arguments())
            *op->add_arguments() = flow.value(argument);
        } else if (const auto *call = dyn_cast<CallExpr>(stmt)) {
          auto assignment = flow.recordAssignments.find(call);
          if (assignment != flow.recordAssignments.end()) {
            const auto *operation = clang::cast<CXXOperatorCallExpr>(call);
            for (auto slot : flow.objects.find(assignment->second)->second.fields) {
              auto *op = target->add_operations(); op->set_kind(Operation::IO_ASSIGN); op->set_local(slot);
              *op->mutable_value() = flow.value(operation->getArg(1));
            }
            continue;
          }
          const auto *observedCallee = exactDirectCallee(call);
          const auto observed = observedCallee ? callerCensus.find(observedCallee->getCanonicalDecl()) : callerCensus.end();
          if (!censusOpaque && observed != callerCensus.end() && !observed->second.escaped) {
            const auto *callee = observedCallee;
            auto *observation = target->add_operations();
            observation->set_kind(Operation::IO_OBSERVE_CALL);
            computeFuncEntityId(const_cast<FunctionDecl *>(callee), ctx,
                                ctx.getSourceManager(), observation->mutable_callee());
            for (const auto *argument : call->arguments()) {
              *observation->add_arguments() = flow.value(argument);
              observation->add_borrowed_inputs();
            }
            auto inputs = flow.readonlyCalls.find(call);
            if (inputs != flow.readonlyCalls.end())
              for (const auto &input : inputs->second) *observation->mutable_borrowed_inputs(input.argument) = flow.borrowedValue(input);
            auto output = flow.borrowedCalls.find(call);
            if (output != flow.borrowedCalls.end()) *observation->mutable_borrowed_inputs(output->second.argument) = flow.borrowedValue(output->second);
          }
          auto recordOutput = flow.recordOutputCalls.find(call);
          if (recordOutput != flow.recordOutputCalls.end()) {
            const auto &object = flow.objects.find(recordOutput->second.first)->second;
            auto *op = target->add_operations();
            op->set_kind(Operation::IO_RECORD_OUTPUT);
            computeFuncEntityId(const_cast<FunctionDecl *>(observedCallee), ctx, ctx.getSourceManager(), op->mutable_callee());
            op->set_output_argument_index(recordOutput->second.second);
            op->set_constructor_record_identity(recordIdentity(object.record, ctx));
            for (auto slot : object.fields) op->add_destination_locals(slot);
            for (unsigned i = 0; i < call->getNumArgs(); ++i) {
              auto *argument = op->add_arguments();
              if (i != recordOutput->second.second) *argument = flow.value(call->getArg(i));
            }
          }
          auto getter = flow.getterCalls.find(call);
          if (getter != flow.getterCalls.end()) {
            const auto &object = flow.objects.find(getter->second)->second;
            auto *op = target->add_operations();
            op->set_kind(Operation::IO_RECEIVER_CALL);
            computeFuncEntityId(
                const_cast<FunctionDecl *>(exactDirectCallee(call)), ctx,
                ctx.getSourceManager(), op->mutable_callee());
            op->set_receiver_record_identity(
                recordIdentity(object.record, ctx));
            auto receiver = flow.recordArgumentCalls.find(call);
            if (receiver != flow.recordArgumentCalls.end()) {
              op->set_receiver_is_parameter(true);
              op->set_receiver_parameter_index(receiver->second);
            }
            for (auto local : object.fields)
              op->add_receiver_locals(local);
            for (unsigned i = 0; i < call->getNumArgs(); ++i) {
              auto *argument = op->add_arguments();
              if (receiver == flow.recordArgumentCalls.end() ||
                  receiver->second != i)
                *argument = flow.value(call->getArg(i));
            }
          }
          auto inputs = flow.readonlyCalls.find(call);
          if (inputs != flow.readonlyCalls.end()) {
            auto *op = target->add_operations();
            op->set_kind(Operation::IO_READONLY_CALL);
            computeFuncEntityId(const_cast<FunctionDecl *>(observedCallee), ctx, ctx.getSourceManager(), op->mutable_callee());
            for (const auto *argument : call->arguments()) {
              *op->add_arguments() = flow.value(argument);
              op->add_borrowed_inputs();
            }
            for (const auto &input : inputs->second) *op->mutable_borrowed_inputs(input.argument) = flow.borrowedValue(input);
          }
          auto found = flow.borrowedCalls.find(call);
          if (found == flow.borrowedCalls.end())
            continue;
          const auto &binding = found->second;
          auto *op = target->add_operations();
          op->set_kind(Operation::IO_CALL);
          computeFuncEntityId(
              const_cast<FunctionDecl *>(exactDirectCallee(call)), ctx,
              ctx.getSourceManager(), op->mutable_callee());
          op->set_has_output_actual(true);
          op->set_output_argument_index(binding.argument);
          op->set_output_actual_is_parameter(binding.parameter);
          if (!binding.parameter) {
            auto local = flow.locals.find(binding.local);
            if (local == flow.locals.end())
              flow.complete = false;
            else
              op->set_output_actual_local(local->second);
          }
          for (unsigned i = 0; i < call->getNumArgs(); ++i) {
            auto *argument = op->add_arguments();
            if (i != binding.argument)
              *argument = flow.value(call->getArg(i));
          }
        } else if (const auto *declarations = dyn_cast<DeclStmt>(stmt)) {
          for (const auto *decl : declarations->decls()) {
            const auto *variable = dyn_cast<VarDecl>(decl);
            auto object = flow.objects.find(variable);
            if (object != flow.objects.end() && !directRecordConstruction(variable->getInit())) {
              for (auto slot : object->second.fields) {
                auto *op = target->add_operations(); op->set_kind(Operation::IO_ASSIGN); op->set_local(slot);
                *op->mutable_value() = flow.value(variable->getInit());
              }
              continue;
            }
            auto found = flow.locals.find(variable);
            if (found == flow.locals.end())
              continue;
            auto *op = target->add_operations();
            op->set_kind(Operation::IO_ASSIGN);
            op->set_local(found->second);
            *op->mutable_value() = flow.value(variable->getInit());
          }
        } else if (const auto *assignment = dyn_cast<BinaryOperator>(stmt)) {
          if (!assignment->isAssignmentOp())
            continue;
          Value assigned = flow.value(assignment->getRHS());
          if (assignment->isCompoundAssignmentOp()) {
            emitRead(assignment, assignment->getLHS());
            assigned.Clear();
            assigned.set_kind(Value::IV_COMBINE);
            *assigned.add_operands() = flow.value(assignment->getLHS());
            *assigned.add_operands() = flow.value(assignment->getRHS());
          }
          if (const auto *field = flow.constructorField(assignment->getLHS())) {
            auto *op = target->add_operations();
            op->set_kind(Operation::IO_FIELD_ASSIGN);
            op->set_local(field->getFieldIndex());
            *op->mutable_value() = assigned;
            continue;
          }
          uint32_t fieldSlot = 0;
          if (flow.objectFieldSlot(assignment->getLHS(), fieldSlot)) {
            auto *op = target->add_operations();
            op->set_kind(Operation::IO_ASSIGN);
            op->set_local(fieldSlot);
            *op->mutable_value() = assigned;
            continue;
          }
          if (flow.outputStorage(assignment->getLHS())) {
            auto *op = target->add_operations();
            op->set_kind(Operation::IO_OUTPUT_ASSIGN);
            *op->mutable_value() = assigned;
            continue;
          }
          const auto *ref = dyn_cast<DeclRefExpr>(
              assignment->getLHS()->IgnoreParenImpCasts());
          auto found = flow.locals.find(ref ? dyn_cast<VarDecl>(ref->getDecl())
                                            : nullptr);
          if (found == flow.locals.end())
            continue;
          auto *op = target->add_operations();
          op->set_kind(Operation::IO_ASSIGN);
          op->set_local(found->second);
          *op->mutable_value() = assigned;
        } else if (const auto *update = dyn_cast<UnaryOperator>(stmt)) {
          if (!update->isIncrementDecrementOp())
            continue;
          emitRead(update, update->getSubExpr());
          if (const auto *field = flow.constructorField(update->getSubExpr())) {
            auto *op = target->add_operations();
            op->set_kind(Operation::IO_FIELD_ASSIGN);
            op->set_local(field->getFieldIndex());
            *op->mutable_value() = flow.value(update->getSubExpr());
            continue;
          }
          uint32_t fieldSlot = 0;
          if (flow.objectFieldSlot(update->getSubExpr(), fieldSlot)) {
            auto *op = target->add_operations();
            op->set_kind(Operation::IO_ASSIGN);
            op->set_local(fieldSlot);
            *op->mutable_value() = flow.value(update->getSubExpr());
            continue;
          }
          if (flow.outputStorage(update->getSubExpr())) {
            auto *op = target->add_operations();
            op->set_kind(Operation::IO_OUTPUT_ASSIGN);
            op->mutable_value()->set_kind(Value::IV_OUTPUT);
            continue;
          }
          const auto *ref = dyn_cast<DeclRefExpr>(
              update->getSubExpr()->IgnoreParenImpCasts());
          auto found = flow.locals.find(ref ? dyn_cast<VarDecl>(ref->getDecl())
                                            : nullptr);
          if (found == flow.locals.end())
            continue;
          auto *op = target->add_operations();
          op->set_kind(Operation::IO_ASSIGN);
          op->set_local(found->second);
          *op->mutable_value() = flow.value(update->getSubExpr());
        } else if (const auto *returned = dyn_cast<ReturnStmt>(stmt)) {
          auto *op = target->add_operations();
          op->set_kind(Operation::IO_RETURN);
          *op->mutable_value() = flow.value(returned->getRetValue());
        }
      }
    }
  for (const auto &read : flow.reads)
    if (!flow.complete || !anchors.count(read.first))
      readFailures[read.first] = flow.complete ? "no-exact-read-anchor" : "unsupported-or-incomplete-function-cfg";
  out->set_complete(flow.complete);
  out->set_own_effects_isolated(flow.ownIsolated && flow.complete);
  buildPointerOrigins(function, ctx, out);
  buildMemoryFacts(function, ctx, out, callSites, accessSites);
  g_facts.push_back(std::move(fact));
}
