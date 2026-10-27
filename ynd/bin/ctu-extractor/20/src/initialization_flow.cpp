#include "initialization_flow.h"

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

bool tracked(QualType type) {
  return !type.isNull() && !type.isVolatileQualified() &&
         (type->isIntegerType() || type->isRealFloatingType() ||
          type->isEnumeralType());
}

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

class Flow : public RecursiveASTVisitor<Flow> {
public:
  bool shouldVisitImplicitCode() const { return true; }
  Flow(FunctionDecl *function, ASTContext &ctx)
      : function(function), ctx(ctx)
  {
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
    const auto *callee = call->getDirectCallee();
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
  struct BorrowedCall {
    unsigned argument = 0;
    const VarDecl *local = nullptr;
    bool parameter = false;
  };
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
    if (!standalone(call) || isa<CXXMethodDecl>(callee) ||
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
    const Expr *actual = call->getArg(binding.argument)->IgnoreParenImpCasts();
    QualType parameterType = callee->getParamDecl(binding.argument)->getType();
    const UnaryOperator *address = nullptr;
    if (parameterType->isPointerType()) {
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
    if (outputStorage(actual))
      binding.parameter = true;
    else {
      const auto *ref = dyn_cast<DeclRefExpr>(actual);
      const auto *variable = ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
      if (!variable || !variable->hasLocalStorage() ||
          variable->getType()->isReferenceType() ||
          !tracked(variable->getType()))
        return false;
      binding.local = variable;
    }
    if (address)
      borrowedAddresses.insert(address);
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
    if (decl != output && decl != receiverParameter &&
        !tracked(decl->getType()) && !supportedConstruction)
      ownIsolated = false;
    if (decl->getType()->isReferenceType() && decl->hasInit())
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
    auto parents = ctx.getParents(*expr);
    if (parents.size() != 1)
      return false;
    if (parents[0].get<CompoundStmt>())
      return true;
    const auto *loop = parents[0].get<ForStmt>();
    return loop && loop->getInc() == expr;
  }
  bool VisitUnaryOperator(UnaryOperator *expr) {
    if (expr->getOpcode() == UO_AddrOf && !borrowedAddresses.count(expr))
      expose(expr->getSubExpr());
    if (expr->getOpcode() == UO_Deref && !outputStorage(expr))
      ownIsolated = false;
    // The value of nested ++/-- would need an expression temporary. A
    // discarded standalone update only preserves its old initialization.
    if (expr->isIncrementDecrementOp()) {
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
    if (expr->getOpcode() != BO_Assign)
      complete = false;
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
    auto parents = ctx.getParents(*expr);
    if (parents.size() != 1 || !parents[0].get<CompoundStmt>())
      complete = false;
    return true;
  }
  bool VisitCallExpr(CallExpr *call) {
    const auto *callee = call->getDirectCallee();
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
      const bool borrowed = recognizeBorrow(call, callee, binding);
      if (borrowed)
        borrowedCalls.emplace(call, binding);
      if ((isa<CXXMethodDecl>(callee) && !getterObject) ||
          callee->isVariadic() ||
          (!tracked(call->getType()) && !call->getType()->isVoidType()))
        ownIsolated = false;
      for (unsigned i = 0; i < callee->getNumParams(); ++i)
        if (!tracked(callee->getParamDecl(i)->getType()) &&
            !(borrowed && i == binding.argument) &&
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
    if (expr->isGLValue() && tracked(expr->getSubExpr()->getType()))
      expose(expr->getSubExpr());
    return true;
  }
  bool VisitCXXConstructExpr(CXXConstructExpr *expr) {
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
    if (outputStorage(expr)) {
      out.set_kind(Value::IV_OUTPUT);
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
      if (found != locals.end() && !poisonAll && !escaped.count(decl)) {
        out.set_kind(Value::IV_LOCAL);
        out.set_index(found->second);
      } else if (isa<EnumConstantDecl>(ref->getDecl())) {
        out.set_kind(Value::IV_DEFINED);
      }
    } else if (const auto *call = dyn_cast<CallExpr>(expr)) {
      auto *callee = call->getDirectCallee();
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
  std::set<const VarDecl *> escapedObjects;
  const ParmVarDecl *output = nullptr;
  unsigned outputIndex = 0;
  std::map<const CallExpr *, BorrowedCall> borrowedCalls;
  std::set<const UnaryOperator *> borrowedAddresses;
  std::map<std::pair<uint64_t, uint64_t>, facts_pb::EntityId> effectCallees;
  unsigned nodes = 0;
  bool poisonAll = false;
  std::set<const VarDecl *> escaped;
  std::map<const VarDecl *, uint32_t> locals;
  std::map<const ImplicitCastExpr *, uint64_t> reads;
};
} // namespace

void tagInitializationRead(const Expr *expr,
                           facts_pb::BaselineCandidate *candidate) {
  const auto found = anchors.find(expr);
  if (found == anchors.end())
    return;
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
  out->set_version(5);
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
  out->set_has_output_parameter(flow.output != nullptr);
  out->set_output_parameter_index(flow.output ? flow.outputIndex : 0);
  out->set_own_effects_isolated(flow.ownIsolated && flow.complete);
  for (const auto &entry : flow.effectCallees)
    *out->add_effect_callees() = entry.second;
  out->set_definition_hash(bodyHash(function, ctx));
  out->set_parameter_count(function->getNumParams());
  out->set_local_count(flow.localCount);
  out->set_scalar_return(tracked(function->getReturnType()));
  setLoc(out->mutable_loc(), function->getLocation(), ctx.getSourceManager());
  if (!cfg)
    flow.complete = false;
  std::set<const ImplicitCastExpr *> emitted;
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
        if (const auto *cast = dyn_cast<ImplicitCastExpr>(stmt)) {
          const auto found = flow.reads.find(cast);
          if (found == flow.reads.end())
            continue;
          if (!emitted.insert(cast).second) {
            flow.complete = false;
            continue;
          }
          auto *op = target->add_operations();
          op->set_kind(Operation::IO_READ);
          op->set_obligation_id(found->second);
          *op->mutable_value() = flow.value(cast->getSubExpr());
          setLoc(op->mutable_loc(), cast->getExprLoc(), ctx.getSourceManager());
          if (!ambiguousAnchors.count(cast)) {
            auto previous = anchors.find(cast);
            if (previous != anchors.end() &&
                (previous->second.first.id() != id.id() ||
                 previous->second.first.id_hi() != id.id_hi() ||
                 previous->second.second != found->second)) {
              anchors.erase(previous);
              ambiguousAnchors.insert(cast);
            } else
              anchors[cast] = {id, found->second};
          }
        } else if (const auto *construction =
                       dyn_cast<CXXConstructExpr>(stmt)) {
          auto found = flow.constructions.find(construction);
          if (found == flow.constructions.end())
            continue;
          const auto &object = flow.objects.find(found->second)->second;
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
          auto getter = flow.getterCalls.find(call);
          if (getter != flow.getterCalls.end()) {
            const auto &object = flow.objects.find(getter->second)->second;
            auto *op = target->add_operations();
            op->set_kind(Operation::IO_RECEIVER_CALL);
            computeFuncEntityId(
                const_cast<FunctionDecl *>(call->getDirectCallee()), ctx,
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
          auto found = flow.borrowedCalls.find(call);
          if (found == flow.borrowedCalls.end())
            continue;
          const auto &binding = found->second;
          auto *op = target->add_operations();
          op->set_kind(Operation::IO_CALL);
          computeFuncEntityId(
              const_cast<FunctionDecl *>(call->getDirectCallee()), ctx,
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
          if (const auto *field = flow.constructorField(assignment->getLHS())) {
            auto *op = target->add_operations();
            op->set_kind(Operation::IO_FIELD_ASSIGN);
            op->set_local(field->getFieldIndex());
            *op->mutable_value() = flow.value(assignment->getRHS());
            continue;
          }
          uint32_t fieldSlot = 0;
          if (flow.objectFieldSlot(assignment->getLHS(), fieldSlot)) {
            auto *op = target->add_operations();
            op->set_kind(Operation::IO_ASSIGN);
            op->set_local(fieldSlot);
            *op->mutable_value() = flow.value(assignment->getRHS());
            continue;
          }
          if (flow.outputStorage(assignment->getLHS())) {
            auto *op = target->add_operations();
            op->set_kind(Operation::IO_OUTPUT_ASSIGN);
            *op->mutable_value() = flow.value(assignment->getRHS());
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
          *op->mutable_value() = flow.value(assignment->getRHS());
        } else if (const auto *update = dyn_cast<UnaryOperator>(stmt)) {
          if (!update->isIncrementDecrementOp())
            continue;
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
  out->set_complete(flow.complete);
  out->set_own_effects_isolated(flow.ownIsolated && flow.complete);
  g_facts.push_back(std::move(fact));
}
