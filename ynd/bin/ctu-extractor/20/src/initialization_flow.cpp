#include "initialization_flow.h"
#include "baseline.h"

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

// A separate may-effect pass: accepting an expression here never supplies an
// initialization or lifetime proof. Unsupported escape operations invalidate the
// entire certificate instead of assuming a borrowed pointer cannot escape.
class BorrowEffects : public RecursiveASTVisitor<BorrowEffects> {
 public:
  BorrowEffects(FunctionDecl *fn, ASTContext &ctx,
                facts_pb::BorrowedEffects *out) : ctx(ctx), out(out) {
    out->set_version(1);
    complete = !isa<CXXMethodDecl>(fn) && !fn->isVariadic() &&
               !fn->isDependentContext() &&
               (tracked(fn->getReturnType()) || fn->getReturnType()->isVoidType() || borrowedType(fn->getReturnType()));
    out->set_returns_borrowed(borrowedType(fn->getReturnType()));
    for (unsigned i = 0; i < fn->getNumParams(); ++i) {
      auto *p = fn->getParamDecl(i);
      parameters[p] = i;
      bool borrowed = borrowedType(p->getType());
      out->add_borrowed_parameters(borrowed);
      out->add_parameter_effects(0);
      if (!borrowed && !tracked(p->getType())) complete = false;
    }
    if (complete) TraverseStmt(fn->getBody());
    for (const auto *ret : returns) {
      auto *origin = out->add_returns();
      const auto *e = ret->getRetValue();
      e = e ? e->IgnoreParenImpCasts() : nullptr;
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
  static bool borrowedType(QualType t) {
    return !t.isVolatileQualified() &&
           (t->isPointerType() || t->isLValueReferenceType()) &&
           tracked(t->getPointeeType());
  }
  const VarDecl *variable(const Expr *e) {
    const auto *ref = e ? dyn_cast<DeclRefExpr>(e->IgnoreParenImpCasts()) : nullptr;
    return ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
  }
  // -2 is private scalar storage; >=0 is a borrowed formal; -3 is unknown.
  int storage(const Expr *e) {
    if (!e) return -3;
    e = e->IgnoreParens();
    if (!tracked(e->getType())) return -3;
    if (const auto *deref = dyn_cast<UnaryOperator>(e)) {
      if (deref->getOpcode() != UO_Deref) return -3;
      const auto *p = variable(deref->getSubExpr());
      auto found = parameters.find(p);
      if (found != parameters.end() && p->getType()->isPointerType() && borrowedType(p->getType()))
        return found->second;
      return -3;
    }
    const auto *v = variable(e);
    if (!v || !v->hasLocalStorage()) return -3;
    auto found = parameters.find(v);
    if (found != parameters.end() && out->borrowed_parameters(found->second)) return found->second;
    return tracked(v->getType()) ? -2 : -3;
  }
  void effect(const Expr *e, unsigned flags) {
    int index = storage(e);
    if (index == -3) complete = false;
    if (index >= 0) out->set_parameter_effects(index, out->parameter_effects(index) | flags);
  }
  bool VisitStmt(Stmt *s) {
    if (++nodes > 4096) { complete = false; return false; }
    if (!isa<CompoundStmt, DeclStmt, ReturnStmt, IfStmt, ForStmt, WhileStmt,
             DoStmt, SwitchStmt, CaseStmt, DefaultStmt, BreakStmt, ContinueStmt,
             NullStmt, DeclRefExpr, IntegerLiteral, FloatingLiteral,
             CharacterLiteral, CXXBoolLiteralExpr, ImplicitCastExpr,
             ParenExpr, UnaryOperator, BinaryOperator, CallExpr,
             ConditionalOperator, InitListExpr, CXXScalarValueInitExpr,
             ImplicitValueInitExpr>(s)) { complete = false; return false; }
    if (const auto *e = dyn_cast<Expr>(s))
      if (e->getType().isNull() || e->getType().isVolatileQualified()) complete = false;
    return true;
  }
  bool VisitVarDecl(VarDecl *v) {
    if (exactFunctionPointerTarget(v)) return true;
    if (!v->hasLocalStorage() || !tracked(v->getType()) || v->hasAttr<CleanupAttr>()) complete = false;
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
        if (const auto *cast = parents[0].get<ImplicitCastExpr>()) { top = cast; continue; }
        if (const auto *paren = parents[0].get<ParenExpr>()) { top = paren; continue; }
        if (const auto *unary = parents[0].get<UnaryOperator>())
          if (unary->getOpcode() == UO_Deref && storage(unary) >= 0) return true;
        break;
      }
      if (!forwardedPointers.count(e)) complete = false;
    }
    return true;
  }
  bool VisitImplicitCastExpr(ImplicitCastExpr *e) {
    if (e->getCastKind() == CK_LValueToRValue && !e->getType()->isPointerType()) effect(e->getSubExpr(), 1);
    if (e->getCastKind() == CK_PointerToIntegral || e->getCastKind() == CK_IntegralToPointer || e->getCastKind() == CK_BitCast || e->getCastKind() == CK_LValueBitCast) complete = false;
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
    if (e->getLHS()->getType()->isPointerType() || e->getRHS()->getType()->isPointerType()) complete = false;
    return true;
  }
  bool VisitReturnStmt(ReturnStmt *s) {
    if (!out->returns_borrowed()) return true;
    returns.push_back(s);
    const auto *e = s->getRetValue();
    e = e ? e->IgnoreParenImpCasts() : nullptr;
    if (const auto *ref = dyn_cast_or_null<DeclRefExpr>(e)) forwardedPointers.insert(ref);
    if (const auto *address = dyn_cast_or_null<UnaryOperator>(e))
      if (address->getOpcode() == UO_AddrOf) borrowedAddresses.insert(address);
    return true;
  }
  bool VisitCallExpr(CallExpr *call) {
    const auto *callee = exactDirectCallee(call);
    if (!callee || isa<CXXMethodDecl>(callee) || callee->isVariadic() ||
        call->getNumArgs() != callee->getNumParams() ||
        (!tracked(call->getType()) && !call->getType()->isVoidType() && !borrowedType(call->getType()))) { complete = false; return true; }
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
  std::map<const CallExpr *, unsigned> callIndices;
  std::vector<const ReturnStmt *> returns;
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
    if (!tracked(field->getType()) || field->isBitField() ||
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
    receiverParameter =
        recordReceiverParameter(function, receiverParameterIndex);
    if (receiverParameter)
      receiverRecord =
          supportedRecord(receiverParameter->getType()->getPointeeType());
    unsigned count = 0;
    bool supported = true;
    for (unsigned i = 0; i < function->getNumParams(); ++i) {
      auto *parameter = function->getParamDecl(i);
      if (parameter == receiverParameter)
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
    if (!member || !isa<CXXThisExpr>(member->getBase()->IgnoreParenImpCasts()))
      return nullptr;
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
    return found != readonlyParameters.end() && parameter->getType()->isPointerType() == pointer ? int(found->second) : -1;
  }
  bool outputStorage(const Expr *expr) const {
    if (!output || !expr)
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
          !tracked(variable->getType()))
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
    const auto *record = decl->hasLocalStorage() && !isa<ParmVarDecl>(decl)
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
    if (supportedConstruction && !objects.count(decl)) {
      Object object;
      object.record = record;
      for (const auto *field : record->fields()) {
        (void)field;
        object.fields.push_back(localCount++);
      }
      objects.emplace(decl, std::move(object));
      constructions.emplace(construction, decl);
    }
    if (!aliases.count(decl) && decl != output && !readonlyParameters.count(dyn_cast<ParmVarDecl>(decl)) && decl != receiverParameter &&
        !tracked(decl->getType()) && !supportedConstruction)
      ownIsolated = false;
    if (decl->getType()->isReferenceType() && decl->hasInit() && !aliases.count(decl))
      expose(decl->getInit());
    if (decl->hasLocalStorage() && tracked(decl->getType()) &&
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
    if (++nodes > 4096) {
      complete = false;
      return false;
    }
    if (isa<AsmStmt, GotoStmt, IndirectGotoStmt, CXXTryStmt, SEHTryStmt,
            LambdaExpr, CoroutineBodyStmt, StmtExpr>(stmt))
      complete = false;
    return true;
  }
  // Escapes permanently poison just their scalar storage. A later assignment
  // cannot restore a proof because a retained alias may write again at any
  // call.
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
      else if (decl->hasLocalStorage() && tracked(decl->getType()))
        escaped.insert(decl);
      // Named globals, non-scalar objects and incoming references are not
      // storage for a newly created local scalar. Any scalar stored in such
      // an object was poisoned at the earlier address/reference binding.
    } else if (const auto *variable = objectBase(expr)) {
      poisonObject(variable);
    } else if (constructorField(expr)) {
      // An escaped subobject can invalidate any later constructor guarantee.
      ownIsolated = false;
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
      uint32_t fieldSlot = 0;
      if (!outputStorage(expr->getSubExpr()) &&
          !constructorField(expr->getSubExpr()) &&
          !objectFieldSlot(expr->getSubExpr(), fieldSlot) &&
          (!variable || !variable->hasLocalStorage() ||
           !tracked(variable->getType()))) {
        ownIsolated = false;
        expose(expr->getSubExpr());
      }
    }
    return true;
  }
  bool VisitBinaryOperator(BinaryOperator *expr) {
    if (!expr->isAssignmentOp())
      return true;
    if (expr->isCompoundAssignmentOp()) {
      reads.emplace(expr, reads.size() + 1);
      // Pointer arithmetic and overloaded operators need separate models.
      if (!tracked(expr->getLHS()->getType()))
        complete = false;
    }
    const auto *leftRef =
        dyn_cast<DeclRefExpr>(expr->getLHS()->IgnoreParenImpCasts());
    const auto *leftVar =
        leftRef ? dyn_cast<VarDecl>(leftRef->getDecl()) : nullptr;
    uint32_t fieldSlot = 0;
    if (!outputStorage(expr->getLHS()) && !constructorField(expr->getLHS()) &&
        !objectFieldSlot(expr->getLHS(), fieldSlot) &&
        (!leftVar || !leftVar->hasLocalStorage() ||
         !tracked(leftVar->getType()))) {
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
            !(recordArgumentCalls.count(call) && i == recordIndex))
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
            !(recordArgumentCalls.count(call) && i == recordIndex)) {
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
    if (expr->isGLValue() && tracked(expr->getSubExpr()->getType()) && !borrowedCasts.count(expr))
      expose(expr->getSubExpr());
    return true;
  }
  bool VisitCXXConstructExpr(CXXConstructExpr *expr) {
    if (copies.count(expr)) return true;
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
  Value value(const Expr *expr, unsigned depth = 0) {
    Value out;
    out.set_unknown_reason(!expr ? "no-initializer" : depth > 128 ? "expression-budget-exhausted" : "unsupported-value-expression");
    if (!expr || depth > 128)
      return out;
    expr = expr->IgnoreParens();
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
    if (tracked(expr->getType()) &&
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
      if (tracked(cast->getType()) && tracked(cast->getSubExpr()->getType()))
        return value(cast->getSubExpr(), depth + 1);
      return out;
    }
    if (isa<IntegerLiteral, FloatingLiteral, CharacterLiteral,
            CXXBoolLiteralExpr>(expr)) {
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
      if (!callee || !tracked(call->getType()) ||
          (isa<CXXMethodDecl>(callee) && getter == getterCalls.end()) ||
          callee->isVariadic() || call->getNumArgs() != callee->getNumParams())
        return out;
      out.set_kind(Value::IV_CALL);
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
      if (!binary->isAssignmentOp() && tracked(binary->getType())) {
        out.set_kind(Value::IV_COMBINE);
        *out.add_operands() = value(binary->getLHS(), depth + 1);
        *out.add_operands() = value(binary->getRHS(), depth + 1);
      }
    } else if (const auto *unary = dyn_cast<UnaryOperator>(expr)) {
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
  std::map<const ParmVarDecl *, unsigned> readonlyParameters;
  std::map<const CallExpr *, std::vector<BorrowedCall>> readonlyCalls;
  FunctionDecl *function;
  ASTContext &ctx;
  bool complete = true;
  bool ownIsolated = true;
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
  unsigned nodes = 0;
  bool poisonAll = false;
  std::set<const CastExpr *> borrowedCasts;
  std::set<const VarDecl *> escaped;
  std::map<const VarDecl *, uint32_t> locals;
  std::map<const Expr *, uint64_t> reads;
};
} // namespace

void collectInitializationCallers(ASTContext &ctx) {
  callerCensus.clear();
  censusOpaque = false;
  CallerVisitor(ctx).TraverseDecl(ctx.getTranslationUnitDecl());
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
                            const facts_pb::EntityId &id) {
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
  out->set_version(8);
  const auto census = callerCensus.find(function->getCanonicalDecl());
  if (!censusOpaque && !function->isExternallyVisible() &&
      !isa<CXXMethodDecl>(function) && !function->isVariadic() &&
      !function->hasAttrs() && !function->getTemplatedKind() &&
      census != callerCensus.end() && !census->second.escaped &&
      !census->second.callers.empty() && census->second.callers.size() <= 256) {
    bool scalar = true;
    for (const auto *parameter : function->parameters())
      scalar = scalar && (tracked(parameter->getType()) || parameter == flow.output || flow.readonlyParameters.count(parameter));
    if (scalar) {
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
  out->set_is_constructor(flow.constructorRecord != nullptr);
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
  out->set_local_count(flow.localCount);
  out->set_scalar_return(tracked(function->getReturnType()));
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
          auto found = flow.constructions.find(construction);
          if (found == flow.constructions.end())
            continue;
          const auto &object = flow.objects.find(found->second)->second;
          auto copy = flow.copies.find(construction);
          if (copy != flow.copies.end()) {
            emitRead(construction, construction);
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
  g_facts.push_back(std::move(fact));
}
