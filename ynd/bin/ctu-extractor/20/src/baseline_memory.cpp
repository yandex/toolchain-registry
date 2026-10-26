// Conservative review candidates, not proven violations. In particular, lack
// of an interprocedural lifetime/initialization proof must not imply safety.
#include "baseline.h"
#include "baseline_pointer_proof.h"
#include "baseline_initialization_proof.h"
#include "baseline_library_proof.h"
#include "baseline_lifetime_proof.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprConcepts.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace {
bool memoryStandardNamespace(const clang::DeclContext *context) {
  for (; context; context = context->getParent())
    if (const auto *space = llvm::dyn_cast<clang::NamespaceDecl>(context))
      if (space->isStdNamespace()) return true;
  return false;
}
bool standardStringType(clang::QualType type) {
  if (type.isNull()) return false;
  const auto *record = type.getNonReferenceType()->getAsCXXRecordDecl();
  return record && record->getName() == "basic_string" && memoryStandardNamespace(record->getDeclContext());
}
bool fixedArrayType(clang::QualType type) {
  type = type.getNonReferenceType();
  if (type->isConstantArrayType()) return true;
  const auto *record = type->getAsCXXRecordDecl();
  return record && record->getName() == "array" && memoryStandardNamespace(record->getDeclContext());
}
const clang::VarDecl *directVariable(const clang::Expr *expr) {
  if (!expr) return nullptr;
  expr = expr->IgnoreParenImpCasts();
  if (const auto *cast = llvm::dyn_cast<clang::ExplicitCastExpr>(expr))
    if (cast->isGLValue() && cast->getType().getCanonicalType().getUnqualifiedType() ==
        cast->getSubExpr()->getType().getCanonicalType().getUnqualifiedType()) return directVariable(cast->getSubExpr());
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr))
    return llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(expr)) {
    const auto *callee = call->getDirectCallee();
    if (callee && callee->getIdentifier() && memoryStandardNamespace(callee->getDeclContext()) &&
        (callee->getName() == "forward" || callee->getName() == "move") && call->getNumArgs() == 1)
      return directVariable(call->getArg(0));
  }
  return nullptr;
}

bool standardScalarParameter(const clang::VarDecl *var) {
  if (!var || var->getType()->isReferenceType()) return false;
    if (llvm::isa<clang::ParmVarDecl>(var))
      if (const auto *type = llvm::dyn_cast<clang::TemplateTypeParmType>(var->getType().getTypePtr()))
        if (const auto *parameter = type->getDecl())
          if (const auto *constraint = parameter->getTypeConstraint()) {
            const auto *namedConcept = constraint->getNamedConcept();
            if (namedConcept && memoryStandardNamespace(namedConcept->getDeclContext())) {
              const auto name = namedConcept->getName();
              if (name == "floating_point" || name == "integral" ||
                  name == "signed_integral" || name == "unsigned_integral") return true;
            }
          }
  return false;
}

bool systemStreamOperation(const clang::FunctionDecl *function, clang::ASTContext &context) {
  if (!function || !function->getIdentifier() || !function->isExternC() ||
      !context.getSourceManager().isInSystemHeader(function->getLocation())) return false;
  const auto name = function->getName();
  return name == "fread" || name == "fwrite" || name == "fgetc" || name == "fputc" ||
      name == "getc" || name == "putc" || name == "fgets" || name == "fputs" ||
      name == "fprintf" || name == "fscanf" || name == "vfprintf" || name == "vfscanf" ||
      name == "ungetc" || name == "fgetwc" || name == "fputwc" || name == "fgetws" ||
      name == "fputws" || name == "fwprintf" || name == "fwscanf" || name == "vfwprintf" ||
      name == "vfwscanf" || name == "getwc" || name == "putwc" || name == "ungetwc" ||
      name == "fclose" || name == "clearerr" || name == "feof" || name == "ferror" ||
      name == "fflush" || name == "fseek" || name == "fsetpos" || name == "rewind" ||
      name == "ftell" || name == "fgetpos" || name == "fileno" || name == "setbuf" || name == "setvbuf";
}

// Collect escape/mutation evidence before proving properties. A direct local
// access is not a pointee access; escaped or reference-bound objects are kept
// conservative because callees can end their lifetime or alter their storage.
class MemoryProofInputs : public clang::RecursiveASTVisitor<MemoryProofInputs> {
 public:
  explicit MemoryProofInputs(clang::ASTContext &context) : Context(context) {}
  clang::ASTContext &Context;
  llvm::DenseSet<const clang::VarDecl *> OpaquePointerUses;
  bool OpaqueAssembly = false;
  bool VisitAsmStmt(clang::AsmStmt *statement) {
    if (!Context.getSourceManager().isInSystemHeader(statement->getAsmLoc())) OpaqueAssembly = true;
    return true;
  }
  void opaquePointer(const clang::Expr *expression) {
    if (!expression) return;
    if (const auto *variable = directVariable(expression))
      if (variable->getType()->isPointerType() || variable->getType()->isArrayType()) OpaquePointerUses.insert(variable);
    expression = expression->IgnoreParenImpCasts();
    if (const auto *choice = llvm::dyn_cast<clang::ConditionalOperator>(expression)) {
      opaquePointer(choice->getTrueExpr()); opaquePointer(choice->getFalseExpr());
    }
    if (const auto *comma = llvm::dyn_cast<clang::BinaryOperator>(expression))
      if (comma->getOpcode() == clang::BO_Comma) opaquePointer(comma->getRHS());
    if (const auto *cast = llvm::dyn_cast<clang::ExplicitCastExpr>(expression)) opaquePointer(cast->getSubExpr());
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expression)) opaquePointer(unary->getSubExpr());
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(expression)) {
      opaquePointer(binary->getLHS()); opaquePointer(binary->getRHS());
    }
    if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(expression)) opaquePointer(subscript->getBase());
  }
  llvm::DenseSet<const clang::VarDecl *> Escaped, Mutated, BoundedRangeIterators, RangeBindings, ConstantRangeValues;
  llvm::DenseMap<const clang::VarDecl *, const clang::VarDecl *> ViewRangeOwners;
  llvm::DenseSet<const clang::VarDecl *> NonCaptureEscapes, UnknownScalarMutations;
  llvm::DenseMap<const clang::VarDecl *, llvm::SmallVector<const clang::Expr *, 2>> ScalarAssignments;
  void assignment(const clang::Expr *left, const clang::Expr *right) {
    if (!left) return;
    if (const auto *variable = directVariable(left)) {
      if (right) ScalarAssignments[variable].push_back(right);
      else UnknownScalarMutations.insert(variable);
    }
    left = left->IgnoreParenImpCasts();
    if (const auto *choice = llvm::dyn_cast<clang::ConditionalOperator>(left)) {
      assignment(choice->getTrueExpr(), right); assignment(choice->getFalseExpr(), right);
    }
    if (const auto *comma = llvm::dyn_cast<clang::BinaryOperator>(left))
      if (comma->getOpcode() == clang::BO_Comma) assignment(comma->getRHS(), right);
  }

  llvm::DenseSet<const clang::LambdaExpr *> ImmediateLambdas;
  llvm::DenseMap<const clang::VarDecl *, llvm::SmallVector<const clang::LambdaExpr *, 2>> ReferenceCaptures;
  bool onlyImmediateCapture(const clang::VarDecl *variable) const {
    const auto found = ReferenceCaptures.find(variable);
    if (NonCaptureEscapes.count(variable) || found == ReferenceCaptures.end()) return false;
    for (const auto *lambda : found->second) if (!ImmediateLambdas.count(lambda)) return false;
    return !found->second.empty();
  }
  llvm::DenseMap<const clang::ParmVarDecl *, llvm::SmallVector<const clang::Expr *, 2>> ImmediateLambdaArguments;
  bool shouldVisitTemplateInstantiations() const { return true; }
  bool shouldVisitImplicitCode() const { return true; }
  void escape(const clang::Expr *expr) {
    if (!expr) return;
    if (auto *v = directVariable(expr)) { Escaped.insert(v); NonCaptureEscapes.insert(v); }
    expr = expr->IgnoreParenImpCasts();
    if (const auto *cast = llvm::dyn_cast<clang::ExplicitCastExpr>(expr))
      if (cast->isGLValue()) escape(cast->getSubExpr());
    if (const auto *choice = llvm::dyn_cast<clang::ConditionalOperator>(expr)) {
      escape(choice->getTrueExpr()); escape(choice->getFalseExpr());
    }
    if (const auto *comma = llvm::dyn_cast<clang::BinaryOperator>(expr))
      if (comma->getOpcode() == clang::BO_Comma) escape(comma->getRHS());
  }
  bool VisitCXXConstructExpr(clang::CXXConstructExpr *expression) {
    const auto *constructor = expression->getConstructor();
    if (constructor && constructor->isTrivial() && constructor->isCopyOrMoveConstructor()) return true;
    const bool stringCopy = constructor && constructor->isCopyOrMoveConstructor() &&
        standardStringType(Context.getRecordType(constructor->getParent()));
    for (unsigned i = 0; i < expression->getNumArgs(); ++i) {
      const auto *argument = expression->getArg(i);
      // A standard string copy/move preserves its source object's lifetime;
      // backing character storage is tracked independently.
      const auto *record = constructor ? constructor->getParent() : nullptr;
      bool filesystemNamespace = false;
      for (const auto *context = record ? record->getDeclContext() : nullptr; context; context = context->getParent())
        if (const auto *space = llvm::dyn_cast<clang::NamespaceDecl>(context))
          filesystemNamespace |= space->getName() == "filesystem";
      const bool pathPointerSource = i == 0 && record && record->getIdentifier() && record->getName() == "path" &&
          memoryStandardNamespace(record->getDeclContext()) && argument->getType()->isPointerType() &&
          argument->getType()->getPointeeType()->isCharType() && filesystemNamespace;
      if (!(stringCopy && i == 0) && !pathPointerSource && argument->isGLValue()) escape(argument);
      opaquePointer(argument);
    }
    return true;
  }
  void mutate(const clang::Expr *expression) {
    if (!expression) return;
    if (const auto *variable = directVariable(expression)) Mutated.insert(variable);
    expression = expression->IgnoreParenImpCasts();
    if (const auto *cast = llvm::dyn_cast<clang::ExplicitCastExpr>(expression))
      if (cast->isGLValue()) mutate(cast->getSubExpr());
    if (const auto *choice = llvm::dyn_cast<clang::ConditionalOperator>(expression)) {
      mutate(choice->getTrueExpr()); mutate(choice->getFalseExpr());
    }
    if (const auto *comma = llvm::dyn_cast<clang::BinaryOperator>(expression))
      if (comma->getOpcode() == clang::BO_Comma) mutate(comma->getRHS());
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expression))
      if (!member->isArrow()) mutate(member->getBase());
  }
  bool VisitExplicitCastExpr(clang::ExplicitCastExpr *expression) {
    if (expression->isGLValue()) escape(expression->getSubExpr());
    return true;
  }
  bool VisitInitListExpr(clang::InitListExpr *expression) {
    for (const auto *initializer : expression->inits()) {
      if (initializer->isGLValue()) escape(initializer);
      opaquePointer(initializer);
    }
    return true;
  }
  bool VisitUnaryOperator(clang::UnaryOperator *expr) {
    if (expr->getOpcode() == clang::UO_AddrOf) { escape(expr->getSubExpr()); opaquePointer(expr->getSubExpr()); }
    if (expr->isIncrementDecrementOp()) { mutate(expr->getSubExpr()); assignment(expr->getSubExpr(), nullptr); }
    return true;
  }
  bool VisitBinaryOperator(clang::BinaryOperator *expr) {
    if (expr->isAssignmentOp()) {
      mutate(expr->getLHS()); opaquePointer(expr->getRHS());
      assignment(expr->getLHS(), expr->isCompoundAssignmentOp() ? nullptr : expr->getRHS());
    }
    return true;
  }
  bool VisitCXXForRangeStmt(clang::CXXForRangeStmt *statement) {
    // Compiler-generated pointer iteration over a fixed array begins at its
    // first element and tests against its one-past end before advancing.
    // This proves the iterator bounds, not the lifetime of an arbitrary owner.
    if (!statement->getRangeInit()) return true;
    // A temporary initializer_list whose elements are constant expressions
    // supplies initialized scalar values. This does not prove pointee contents.
    const clang::Expr *rangeValue = statement->getRangeInit();
    bool standardList = false;
    for (unsigned depth = 0; rangeValue && depth != 8; ++depth) {
      rangeValue = rangeValue->IgnoreParenImpCasts();
      if (const auto *list = llvm::dyn_cast<clang::CXXStdInitializerListExpr>(rangeValue)) {
        standardList = true; rangeValue = list->getSubExpr();
      } else if (const auto *temporary = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(rangeValue)) rangeValue = temporary->getSubExpr();
      else if (const auto *cleanup = llvm::dyn_cast<clang::ExprWithCleanups>(rangeValue)) rangeValue = cleanup->getSubExpr();
      else break;
    }
    if (standardList)
      if (const auto *list = llvm::dyn_cast_or_null<clang::InitListExpr>(rangeValue)) {
        bool initialized = !list->isValueDependent();
        for (const auto *element : list->inits()) initialized &= element->isCXX11ConstantExpr(Context);
        if (initialized) ConstantRangeValues.insert(statement->getLoopVariable());
      }
    const bool fixed = fixedArrayType(statement->getRangeInit()->getType());
    const auto *owner = directVariable(statement->getRangeInit());
    const auto *record = owner ? owner->getType()->getAsCXXRecordDecl() : nullptr;
    const bool ownedView = owner && !owner->getType()->isReferenceType() && record &&
        record->getName() == "basic_string_view" && memoryStandardNamespace(record->getDeclContext());
    if (!fixed && !ownedView) return true;
    if (const auto *range = statement->getRangeStmt())
      if (range->isSingleDecl())
        if (const auto *binding = llvm::dyn_cast<clang::VarDecl>(range->getSingleDecl())) RangeBindings.insert(binding);
    for (const auto *declarations : {statement->getBeginStmt(), statement->getEndStmt()}) {
      if (!declarations || !declarations->isSingleDecl()) continue;
      const auto *variable = llvm::dyn_cast<clang::VarDecl>(declarations->getSingleDecl());
      if (variable && variable->getType()->isPointerType()) {
        if (fixed) BoundedRangeIterators.insert(variable);
        else ViewRangeOwners[variable] = owner;
      }
    }
    return true;
  }
  bool VisitLambdaExpr(clang::LambdaExpr *expression) {
    for (const auto &capture : expression->captures())
      if (capture.capturesVariable() && capture.getCaptureKind() == clang::LCK_ByRef) {
        if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(capture.getCapturedVar())) {
          Escaped.insert(variable);
          ReferenceCaptures[variable].push_back(expression);
        } else if (const auto *binding = llvm::dyn_cast<clang::BindingDecl>(capture.getCapturedVar()))
          if (const auto *variable = llvm::dyn_cast_or_null<clang::VarDecl>(binding->getDecomposedDecl())) {
            Escaped.insert(variable);
            NonCaptureEscapes.insert(variable); // decomposed subobject aliases remain unknown
          }
      }
    return true;
  }
  bool VisitCXXPseudoDestructorExpr(clang::CXXPseudoDestructorExpr *expr) {
    escape(expr->getBase()); opaquePointer(expr->getBase());
    return true;
  }
  bool VisitVarDecl(clang::VarDecl *decl) {
    if (decl->hasInit()) opaquePointer(decl->getInit());
    // The compiler's hidden range binding only keeps the range alive; user
    // aliases/captures still count as escapes in their own AST nodes.
    if (decl->getType()->isReferenceType() && !RangeBindings.count(decl)) escape(decl->getInit());
    return true;
  }
  bool VisitReturnStmt(clang::ReturnStmt *statement) {
    if (statement->getRetValue() && statement->getRetValue()->isGLValue()) escape(statement->getRetValue());
    return true;
  }
  bool discardedExpression(const clang::Expr *expression) const {
    clang::DynTypedNode node = clang::DynTypedNode::create(*expression);
    for (unsigned depth = 0; depth != 8; ++depth) {
      const auto parents = Context.getParents(node);
      if (parents.size() != 1) return false;
      const auto &parent = parents[0];
      if (parent.get<clang::CompoundStmt>()) return true;
      if (!parent.get<clang::ExprWithCleanups>() && !parent.get<clang::ParenExpr>()) return false;
      node = parent;
    }
    return false;
  }
  bool readOnlyReferenceParameter(const clang::FunctionDecl *function, unsigned index,
                                  bool discardedReference, unsigned depth = 0) const {
    if (!function || depth > 5) return false;
    const auto *definition = function->getDefinition();
    if (!definition || index >= definition->getNumParams() || !definition->getBody()) return false;
    const auto *parameter = definition->getParamDecl(index);
    if (!parameter->getType()->isReferenceType() ||
        !parameter->getType()->getPointeeType()->isScalarType()) return false;
    struct Uses : clang::RecursiveASTVisitor<Uses> {
      const clang::ParmVarDecl *Parameter;
      llvm::SmallVector<const clang::DeclRefExpr *, 8> References;
      bool Assembly = false;
      explicit Uses(const clang::ParmVarDecl *parameter) : Parameter(parameter) {}
      bool VisitDeclRefExpr(clang::DeclRefExpr *reference) {
        if (reference->getDecl() == Parameter) References.push_back(reference);
        return References.size() < 128;
      }
      bool VisitAsmStmt(clang::AsmStmt *) { Assembly = true; return true; }
      bool VisitLambdaExpr(clang::LambdaExpr *lambda) {
        for (const auto &capture : lambda->captures())
          if (capture.capturesVariable() && capture.getCapturedVar() == Parameter) Assembly = true;
        return true;
      }
    } uses(parameter);
    if (!uses.TraverseStmt(const_cast<clang::Stmt *>(definition->getBody())) || uses.Assembly) return false;
    for (const auto *reference : uses.References) {
      clang::DynTypedNode node = clang::DynTypedNode::create(*reference);
      bool safe = false;
      for (unsigned steps = 0; steps != 12; ++steps) {
        const auto parents = Context.getParents(node);
        if (parents.size() != 1) break;
        const auto &parent = parents[0];
        if (const auto *cast = parent.get<clang::ImplicitCastExpr>()) {
          if (cast->getCastKind() == clang::CK_LValueToRValue) { safe = true; break; }
          if (cast->getCastKind() != clang::CK_NoOp) break;
          node = parent; continue;
        }
        if (parent.get<clang::ParenExpr>()) { node = parent; continue; }
        if (const auto *call = parent.get<clang::CallExpr>()) {
          const auto *callee = call->getDirectCallee();
          if (callee && callee->getIdentifier() && memoryStandardNamespace(callee->getDeclContext()) &&
              (callee->getName() == "forward" || callee->getName() == "move")) { node = parent; continue; }
          for (unsigned i = 0; i < call->getNumArgs(); ++i)
            if (call->getArg(i) == node.get<clang::Expr>())
              safe = readOnlyReferenceParameter(callee, i,
                  callee && callee->getReturnType()->isReferenceType() && discardedExpression(call), depth + 1);
          break;
        }
        if (parent.get<clang::ReturnStmt>()) {
          safe = discardedReference && definition->getReturnType()->isReferenceType(); break;
        }
        if (parent.get<clang::UnaryExprOrTypeTraitExpr>()) { safe = true; break; }
        break;
      }
      if (!safe) return false;
    }
    return true;
  }
  bool readOnlyReferenceArgument(const clang::CallExpr *call, unsigned index) const {
    if (llvm::isa<clang::CXXOperatorCallExpr, clang::CXXMemberCallExpr>(call)) return false;
    const auto *argument = directVariable(call->getArg(index));
    if (!argument || !argument->getType()->isScalarType()) return false;
    const auto *callee = call->getDirectCallee();
    return readOnlyReferenceParameter(callee, index,
        callee && callee->getReturnType()->isReferenceType() && discardedExpression(call));
  }
  bool scalarByValueArgument(const clang::CallExpr *call, unsigned index) const {
    // An arithmetic-constrained dependent value has no associated namespace
    // for ADL. If every visible overload takes its sole argument by value,
    // template parsing's missing lvalue-to-rvalue cast is not an escape.
    if (call->getNumArgs() != 1 || index != 0 || !standardScalarParameter(directVariable(call->getArg(0)))) return false;
    const auto *lookup = llvm::dyn_cast<clang::UnresolvedLookupExpr>(call->getCallee()->IgnoreParenImpCasts());
    if (!lookup || lookup->hasExplicitTemplateArgs() || lookup->decls_begin() == lookup->decls_end()) return false;
    for (const auto *decl : lookup->decls()) {
      const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl);
      if (const auto *templ = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) function = templ->getTemplatedDecl();
      if (!function || function->getNumParams() != 1 || function->isVariadic()) return false;
      const auto type = function->getParamDecl(0)->getType();
      if (type->isReferenceType() || (!type->isScalarType() && !llvm::isa<clang::TemplateTypeParmType>(type.getTypePtr()))) return false;
    }
    return true;
  }
  bool VisitCallExpr(clang::CallExpr *expr) {
    if (!systemStreamOperation(expr->getDirectCallee(), Context))
      for (const auto *argument : expr->arguments()) {
        const auto *function = expr->getDirectCallee();
        const auto *array = directVariable(argument);
        // These C routines access character storage without retaining it or
        // ending the array's lifetime. Bounds and initialized bytes are separate.
        const bool characterArray = array && array->getType()->isArrayType() &&
            Context.getBaseElementType(array->getType())->isCharType();
        const bool storageOnly = characterArray && function && function->getIdentifier() && function->isExternC() &&
            Context.getSourceManager().isInSystemHeader(function->getLocation()) &&
            (function->getName() == "sscanf" || function->getName() == "__isoc99_sscanf" ||
             function->getName() == "strlen" || function->getName() == "strcmp" || function->getName() == "strncmp");
        if (!storageOnly) opaquePointer(argument);
      }
    if (const auto *member = llvm::dyn_cast<clang::CXXMemberCallExpr>(expr)) {
      if (llvm::isa_and_nonnull<clang::CXXDestructorDecl>(member->getDirectCallee()))
        escape(member->getImplicitObjectArgument());
      if (member->getMethodDecl() && !member->getMethodDecl()->isConst())
        mutate(member->getImplicitObjectArgument());
    }
    const auto *directCallee = expr->getDirectCallee();
    const auto *stringMethod = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(directCallee);
    const bool standardStringMethod = stringMethod &&
        standardStringType(Context.getRecordType(stringMethod->getParent()));
    const bool ownerReferenceResult = standardStringMethod && stringMethod->getReturnType()->isReferenceType() &&
        standardStringType(stringMethod->getReturnType());
    const bool receiverPreserved = standardStringMethod && !llvm::isa<clang::CXXDestructorDecl>(stringMethod) &&
        (!ownerReferenceResult || discardedExpression(expr));
    if (standardStringMethod && !receiverPreserved)
      if (const auto *member = llvm::dyn_cast<clang::CXXMemberCallExpr>(expr)) escape(member->getImplicitObjectArgument());
    const bool forwarding = directCallee && directCallee->getIdentifier() &&
        memoryStandardNamespace(directCallee->getDeclContext()) &&
        (directCallee->getName() == "forward" || directCallee->getName() == "move");
    const auto *operatorCall = llvm::dyn_cast<clang::CXXOperatorCallExpr>(expr);
    const auto *operatorMethod = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(directCallee);
    const bool atomicAssignment = operatorCall && operatorMethod &&
        operatorCall->getOperator() == clang::OO_Equal &&
        (operatorMethod->getParent()->getName() == "atomic" ||
         operatorMethod->getParent()->getName() == "__atomic_base") &&
        memoryStandardNamespace(operatorMethod->getDeclContext());
    if (!forwarding)
      for (unsigned i = 0; i < expr->getNumArgs(); ++i)
        // std::atomic::operator= changes its value without exposing its owner.
        if (!(atomicAssignment && i == 0) &&
            !(operatorCall && i == 0 && receiverPreserved) &&
            !(standardStringMethod && (!operatorCall || i != 0) && standardStringType(expr->getArg(i)->getType())) &&
            expr->getArg(i)->isGLValue() && !scalarByValueArgument(expr, i) &&
            !readOnlyReferenceArgument(expr, i)) escape(expr->getArg(i));
    if (const auto *call = llvm::dyn_cast<clang::CXXOperatorCallExpr>(expr)) {
      const auto *method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(call->getDirectCallee());
      if (method && method->getParent()->isLambda() && call->getNumArgs() == method->getNumParams() + 1) {
        const clang::Expr *receiver = call->getArg(0)->IgnoreParenImpCasts();
        if (auto *temporary = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(receiver))
          receiver = temporary->getSubExpr()->IgnoreParenImpCasts();
        if (const auto *lambda = llvm::dyn_cast<clang::LambdaExpr>(receiver)) {
          ImmediateLambdas.insert(lambda);
          auto record = [&](const clang::FunctionDecl *declaration) {
            if (!declaration || declaration->getNumParams() != method->getNumParams()) return;
            for (unsigned i = 0; i < method->getNumParams(); ++i)
              ImmediateLambdaArguments[declaration->getParamDecl(i)].push_back(call->getArg(i + 1));
          };
          record(method);
          if (const auto *primary = method->getPrimaryTemplate()) record(primary->getTemplatedDecl());
        }
      }
    }
    return true;
  }
};

class MemoryBaselineVisitor : public clang::RecursiveASTVisitor<MemoryBaselineVisitor> {
 public:
  explicit MemoryBaselineVisitor(clang::ASTContext &context, const MemoryProofInputs &proofs) : Context(context), Proofs(proofs), GlobalInitialization(context) {}
  bool shouldVisitTemplateInstantiations() const { return true; }
  bool shouldVisitImplicitCode() const { return true; }

  bool VisitDeclRefExpr(clang::DeclRefExpr *expr) {
    // Reference binding, address escape and non-reading uses do not necessarily
    // produce an lvalue-to-rvalue cast. Keep lifetime candidates for those too.
    if (llvm::isa<clang::VarDecl>(expr->getDecl()) && !liveDirectObject(expr))
      emit("6.8.1", expr, "object-reference-needs-lifetime-proof");
    return true;
  }

  bool VisitImplicitCastExpr(clang::ImplicitCastExpr *expr) {
    if (expr->getCastKind() != clang::CK_LValueToRValue) return true;
    if (!initializedDirectScalar(expr->getSubExpr()))
      emit("11.6.2", expr, "value-read-needs-initialization-proof");
    if (!liveDirectObject(expr->getSubExpr()))
      emit("6.8.1", expr, "value-read-needs-lifetime-proof");
    if (expr->getType()->isPointerType() && !nonLibraryPointer(expr->getSubExpr()))
      emit("25.5.3", expr, "pointer-use-needs-library-invalidation-proof");
    return true;
  }

  bool VisitUnaryOperator(clang::UnaryOperator *expr) {
    if (expr->getOpcode() == clang::UO_Deref) {
      if (!errnoStorage(expr)) emit("6.8.1", expr, "indirection-needs-lifetime-proof");
      if (!nonLibraryPointer(expr->getSubExpr()))
        emit("25.5.3", expr, "indirection-needs-library-invalidation-proof");
    }
    if (expr->isIncrementDecrementOp()) {
      // Clang does not insert an lvalue-to-rvalue cast for the read implicit in
      // a read/modify/write operation.
      if (!initializedDirectScalar(expr->getSubExpr()))
        emit("11.6.2", expr, "read-modify-write-needs-initialization-proof");
      if (expr->getSubExpr()->getType()->isPointerType()) {
        if (!boundedRangeIterator(directVariable(expr->getSubExpr())) &&
            !baseline_pointer::Proof(Context).safeRangeIncrement(expr))
          emit("8.7.1", expr, "pointer-step-needs-array-bounds-proof");
        if (!nonLibraryPointer(expr->getSubExpr()))
          emit("25.5.3", expr, "pointer-update-needs-library-invalidation-proof");
      }
    }
    return true;
  }

  bool VisitArraySubscriptExpr(clang::ArraySubscriptExpr *expr) {
    const bool bounded = boundedArrayIndex(expr) || baseline_pointer::Proof(Context).boundedSubscript(expr);
    if (!bounded) emit("8.7.1", expr, "subscript-needs-array-bounds-proof");
    if (!(bounded && llvm::isa<clang::StringLiteral>(expr->getBase()->IgnoreParenImpCasts())) &&
        !baseline_lifetime::Proof(Context).live(expr))
      emit("6.8.1", expr, "subscript-needs-lifetime-proof");
    if (!nonLibraryPointer(expr->getBase()))
      emit("25.5.3", expr, "subscript-needs-library-invalidation-proof");
    return true;
  }

  bool VisitMemberExpr(clang::MemberExpr *expr) {
    if (expr->isArrow() || !liveDirectObject(expr->getBase()))
      emit("6.8.1", expr, "member-access-needs-lifetime-proof");
    if (expr->isArrow() && !nonLibraryPointer(expr->getBase()))
      emit("25.5.3", expr, "member-access-needs-library-invalidation-proof");
    return true;
  }

  bool VisitBinaryOperator(clang::BinaryOperator *expr) {
    const bool leftPointer = expr->getLHS()->getType()->isPointerType();
    const bool rightPointer = expr->getRHS()->getType()->isPointerType();
    if ((expr->getOpcode() == clang::BO_Add || expr->getOpcode() == clang::BO_Sub ||
         expr->getOpcode() == clang::BO_AddAssign || expr->getOpcode() == clang::BO_SubAssign) &&
        (leftPointer || rightPointer)) {
      if (expr->getOpcode() == clang::BO_Sub && leftPointer && rightPointer) {
        if (!baseline_pointer::Proof(Context).sameArray(expr->getLHS(), expr->getRHS(), expr))
          emit("8.7.2", expr, "pointer-difference-needs-common-array-proof");
      }
      else if (!baseline_pointer::Proof(Context).boundedPosition(expr))
        emit("8.7.1", expr, "pointer-arithmetic-needs-array-bounds-proof");
    }
    if ((expr->isRelationalOp() || expr->getOpcode() == clang::BO_Cmp) &&
        (leftPointer || rightPointer) &&
        !baseline_pointer::Proof(Context).sameArray(expr->getLHS(), expr->getRHS(), expr))
      emit("8.9.1", expr, "pointer-order-needs-common-array-proof");
    if (expr->isCompoundAssignmentOp()) {
      if (!initializedDirectScalar(expr->getLHS()))
        emit("11.6.2", expr, "read-modify-write-needs-initialization-proof");
      if (leftPointer && !nonLibraryPointer(expr->getLHS()))
        emit("25.5.3", expr, "pointer-update-needs-library-invalidation-proof");
    }
    if (expr->isAssignmentOp()) {
      if (!safeScalarAssignment(expr))
        emit("8.18.1", expr, "assignment-needs-nonoverlap-or-exact-overlap-proof");
      if (!liveDirectObject(expr->getLHS()))
        emit("6.8.1", expr, "assignment-needs-destination-lifetime-proof");
      if (expr->getRHS()->getType()->isRecordType())
        emit("11.6.2", expr, "aggregate-copy-needs-member-initialization-proof");
    }
    return true;
  }

  bool VisitVarDecl(clang::VarDecl *decl) {
    if (decl->getType()->isReferenceType() && decl->hasInit())
      emitBaselineCandidate(Context, "6.8.1", decl->getLocation(), "reference-binding-needs-lifetime-proof");
    return true;
  }

  bool VisitCXXConstructExpr(clang::CXXConstructExpr *expr) {
    if (!expr->getConstructor() || !expr->getConstructor()->hasBody())
      unresolvedEffects(expr->getExprLoc());
    if (expr->getConstructor() && expr->getConstructor()->isCopyOrMoveConstructor()) {
      emit("11.6.2", expr, "aggregate-copy-needs-member-initialization-proof");
      emit("6.8.1", expr, "aggregate-copy-needs-source-lifetime-proof");
      if (!freshCopyDestination(expr))
        emit("8.18.1", expr, "aggregate-copy-needs-nonoverlap-proof");
    }
    return true;
  }

  bool VisitCallExpr(clang::CallExpr *expr) {
    // Definitions can reside in another TU or outside the analyzed program.
    // Report that uncertainty explicitly instead of treating an unknown callee
    // (including a known name with an unavailable implementation) as harmless.
    const auto *callee = expr->getDirectCallee();
    const auto *method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(callee);
    if (method && (method->isCopyAssignmentOperator() || method->isMoveAssignmentOperator()))
      emit("6.8.1", expr, "aggregate-assignment-needs-source-and-destination-lifetime-proof");
    // A visible virtual base body does not establish the effects of the
    // override selected at runtime, which can reside outside this TU.
    const bool unknownEffects = !callee || !callee->hasBody() || (method && method->isVirtual());
    if (unknownEffects) unresolvedEffects(expr->getExprLoc());
    if (callee && callee->getIdentifier()) {
      const auto name = callee->getName();
      if (name == "memcpy" || name == "memmove" || name == "strcpy" ||
          name == "strncpy" || name == "strcat" || name == "strncat" ||
          name == "wmemcpy" || name == "wmemmove" || name == "wcscpy" ||
          name == "wcsncpy" || name == "wcscat" || name == "wcsncat" ||
          name == "__builtin_memcpy" || name == "__builtin_memmove")
        if (!disjointArrayCopy(expr)) emit("8.18.1", expr, "copy-call-needs-nonoverlap-or-exact-overlap-proof");
      if (name == "fread" || name == "fwrite" || name == "fgetc" ||
          name == "fputc" || name == "getc" || name == "putc" ||
          name == "fgets" || name == "fputs" || name == "fprintf" ||
          name == "fscanf" || name == "vfprintf" || name == "vfscanf" ||
          name == "getchar" || name == "putchar" || name == "printf" ||
          name == "scanf" || name == "vprintf" || name == "vscanf" ||
          name == "puts" || name == "ungetc" || name == "fgetwc" ||
          name == "fputwc" || name == "fgetws" || name == "fputws" ||
          name == "fwprintf" || name == "fwscanf" || name == "vfwprintf" ||
          name == "vfwscanf" || name == "getwc" || name == "putwc" ||
          name == "getwchar" || name == "putwchar" || name == "wprintf" ||
          name == "wscanf" || name == "vwprintf" || name == "vwscanf" || name == "ungetwc")
        if (!nonUpdateStream(expr)) emit("30.0.2", expr, "stream-io-needs-direction-transition-proof");
    }
    return true;
  }

  bool VisitAsmStmt(clang::AsmStmt *stmt) {
    unresolvedEffects(stmt->getAsmLoc(), true);
    return true;
  }

 private:
  bool nonUpdateStream(const clang::CallExpr *expression) const {
    const auto *function = expression->getDirectCallee();
    if (Proofs.OpaqueAssembly || !systemStreamOperation(function, Context)) return false;
    const auto name = function->getName();
    unsigned argument;
    if (name == "fread" || name == "fwrite") argument = 3;
    else if (name == "fgets" || name == "fgetws") argument = 2;
    else if (name == "fputc" || name == "putc" || name == "fputs" || name == "ungetc" ||
             name == "fputwc" || name == "putwc" || name == "fputws" || name == "ungetwc") argument = 1;
    else if (name == "fgetc" || name == "getc" || name == "fgetwc" || name == "getwc" ||
             name == "fprintf" || name == "fscanf" || name == "vfprintf" || name == "vfscanf" ||
             name == "fwprintf" || name == "fwscanf" || name == "vfwprintf" || name == "vfwscanf") argument = 0;
    else return false;
    if (argument >= expression->getNumArgs()) return false;
    const auto *variable = directVariable(expression->getArg(argument));
    if (!variable || !variable->hasLocalStorage() || !variable->hasInit() || variable->getType()->isReferenceType() ||
        Proofs.Mutated.count(variable) || Proofs.Escaped.count(variable) || Proofs.OpaquePointerUses.count(variable)) return false;
    const auto *open = llvm::dyn_cast<clang::CallExpr>(variable->getInit()->IgnoreParenImpCasts());
    const auto *opener = open ? open->getDirectCallee() : nullptr;
    if (!opener || !opener->getIdentifier() || opener->getName() != "fopen" || !opener->isExternC() ||
        !Context.getSourceManager().isInSystemHeader(opener->getLocation()) || open->getNumArgs() != 2) return false;
    const auto *mode = llvm::dyn_cast<clang::StringLiteral>(open->getArg(1)->IgnoreParenImpCasts());
    if (!mode || mode->getBytes().empty()) return false;
    const auto bytes = mode->getBytes();
    return (bytes.front() == 'r' || bytes.front() == 'w' || bytes.front() == 'a') && !bytes.contains('+');
  }
  bool boundedRangeIterator(const clang::VarDecl *variable) const {
    if (!variable) return false;
    if (Proofs.BoundedRangeIterators.count(variable)) return true;
    const auto found = Proofs.ViewRangeOwners.find(variable);
    return found != Proofs.ViewRangeOwners.end() && !Proofs.Mutated.count(found->second) &&
        !Proofs.Escaped.count(found->second);
  }
  bool freshCopyDestination(const clang::CXXConstructExpr *expression) const {
    if (expression->getConstructionKind() != clang::CXXConstructionKind::Complete ||
        expression->getNumArgs() == 0) return false;
    const auto *source = directVariable(expression->getArg(0));
    clang::DynTypedNode node = clang::DynTypedNode::create(*expression);
    for (unsigned depth = 0; depth != 10; ++depth) {
      const auto parents = Context.getParents(node);
      if (parents.size() != 1) return false;
      const auto &parent = parents[0];
      if (const auto *variable = parent.get<clang::VarDecl>())
        return variable != source && variable->hasLocalStorage() && !variable->getType()->isReferenceType();
      // The function result is a fresh complete object, or the exact source
      // under permitted elision. Its caller's explicit placement construction
      // is checked at that site; source lifetime/initialization stay separate.
      if (parent.get<clang::ReturnStmt>()) return true;
      // A by-value argument/result temporary is a distinct complete object.
      // Placement construction and base/member subobject construction do not
      // enter this proof and retain their overlap candidate.
      if (parent.get<clang::CallExpr>() || parent.get<clang::CXXConstructExpr>()) return true;
      if (!parent.get<clang::ImplicitCastExpr>() && !parent.get<clang::ParenExpr>() &&
          !parent.get<clang::ExprWithCleanups>() && !parent.get<clang::CXXBindTemporaryExpr>() &&
          !parent.get<clang::MaterializeTemporaryExpr>() && !parent.get<clang::ConditionalOperator>()) return false;
      node = parent;
    }
    return false;
  }
  bool errnoStorage(const clang::Expr *expression) const {
    if (!expression) return false;
    const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expression->IgnoreParenImpCasts());
    if (!unary || unary->getOpcode() != clang::UO_Deref) return false;
    const auto *call = llvm::dyn_cast<clang::CallExpr>(unary->getSubExpr()->IgnoreParenImpCasts());
    const auto *function = call ? call->getDirectCallee() : nullptr;
    return function && function->getIdentifier() && function->getName() == "__errno_location" &&
        function->isExternC() && Context.getSourceManager().isInSystemHeader(function->getLocation());
  }
  bool liveDirectObject(const clang::Expr *expr) const {
    if (errnoStorage(expr)) return true;
    if (expr)
      if (const auto *element = llvm::dyn_cast<clang::ArraySubscriptExpr>(expr->IgnoreParenImpCasts()))
        if (llvm::isa<clang::StringLiteral>(element->getBase()->IgnoreParenImpCasts()) &&
            (boundedArrayIndex(element) || baseline_pointer::Proof(Context).boundedSubscript(element))) return true;
    const auto *var = directVariable(expr);
    if (!var || var->getType()->isReferenceType() ||
        (Proofs.Escaped.count(var) && !Proofs.onlyImmediateCapture(var))) return baseline_lifetime::Proof(Context).live(expr);
    if (var->hasInit() && Context.getSourceManager().isPointWithin(
            expr->getExprLoc(), var->getInit()->getBeginLoc(), var->getInit()->getEndLoc())) return false;
    if (var->hasLocalStorage() && var->getType()->isConstantArrayType() &&
        Context.getBaseElementType(var->getType())->isCharType() &&
        !Proofs.OpaqueAssembly && !Proofs.OpaquePointerUses.count(var)) return true;
    const auto *record = var->getType()->getAsCXXRecordDecl();
    // Standard string operations preserve the local owning object. Escaping
    // owner references and explicit destruction/reuse were rejected above;
    // character buffers and iterators do not inherit this lifetime proof.
    if (var->hasLocalStorage() && standardStringType(var->getType())) return true;
    const bool standardRecord = record && memoryStandardNamespace(record->getDeclContext());
    const bool stringView = standardRecord && record->getName() == "basic_string_view";
    const bool atomic = standardRecord && record->getName() == "atomic";
    // Mutable nonlocals can acquire unseen aliases. Internal atomic objects
    // are exception-safe value storage with no exposing methods; address,
    // reference return, capture and explicit destruction were scanned above.
    if (var->hasGlobalStorage() && !var->getType().isConstQualified() &&
        !(atomic && !var->isExternallyVisible())) return false;
    // string_view::data exposes the viewed buffer, never the view object.
    // Keep its backing pointer/reference operations separate and conservative.
    if ((stringView || atomic) && record->hasDefinition() && record->hasTrivialDestructor()) return true;
    // A standard arithmetic concept constrains even the uninstantiated value
    // parameter to scalar storage. User concepts with similar names and all
    // reference parameters remain unresolved.
    if (standardScalarParameter(var)) return true;
    // Other class objects may expose themselves or have custom destruction.
    return var->getType()->isScalarType() || baseline_lifetime::Proof(Context).live(expr);
  }
  bool initializedValue(const clang::Expr *expr, unsigned depth) const {
    if (!expr || depth > 12) return false;
    if (initializedStandardResultMember(expr)) return true;
    if (expr->isValueDependent()) return false;
    expr = expr->IgnoreParens();
    if (const auto *temporary = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expr))
      return initializedValue(temporary->getSubExpr(), depth + 1);
    if (const auto *cleanup = llvm::dyn_cast<clang::ExprWithCleanups>(expr))
      return initializedValue(cleanup->getSubExpr(), depth + 1);
    if (expr->isCXX11ConstantExpr(Context)) return true;
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(expr)) {
      if (cast->getCastKind() == clang::CK_ArrayToPointerDecay ||
          cast->getCastKind() == clang::CK_FunctionToPointerDecay) return true;
      return initializedValue(cast->getSubExpr(), depth + 1);
    }
    if (const auto *cast = llvm::dyn_cast<clang::ExplicitCastExpr>(expr))
      return initializedValue(cast->getSubExpr(), depth + 1);
    if (llvm::isa<clang::IntegerLiteral>(expr) || llvm::isa<clang::FloatingLiteral>(expr) ||
        llvm::isa<clang::CharacterLiteral>(expr) || llvm::isa<clang::CXXBoolLiteralExpr>(expr) ||
        llvm::isa<clang::StringLiteral>(expr) || llvm::isa<clang::CXXNullPtrLiteralExpr>(expr) ||
        llvm::isa<clang::CXXScalarValueInitExpr>(expr) || llvm::isa<clang::UnaryExprOrTypeTraitExpr>(expr)) return true;
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
      if (unary->getOpcode() == clang::UO_AddrOf) return true;
      if (unary->getOpcode() == clang::UO_Deref) return errnoStorage(unary);
      return initializedValue(unary->getSubExpr(), depth + 1);
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(expr))
      return initializedValue(binary->getLHS(), depth + 1) && initializedValue(binary->getRHS(), depth + 1);
    if (const auto *conditional = llvm::dyn_cast<clang::ConditionalOperator>(expr))
      return initializedValue(conditional->getCond(), depth + 1) &&
          initializedValue(conditional->getTrueExpr(), depth + 1) && initializedValue(conditional->getFalseExpr(), depth + 1);
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(expr))
      return initializedLibraryReturn(call) || GlobalInitialization.initializedValue(call);
    return initializedDirectScalar(expr, depth + 1);
  }
  bool initializedLibraryReturn(const clang::CallExpr *call) const {
    const auto *callee = call->getDirectCallee();
    if (!callee) return false;
    // These interfaces return a defined scalar result on both success and
    // failure. Requiring the system declaration avoids name-only user models.
    if (callee->isExternC() && Context.getSourceManager().isInSystemHeader(callee->getLocation())) {
      const auto name = callee->getName();
      if (name == "syscall" || name == "backtrace" || name == "backtrace_symbols" ||
          name == "fopen" || name == "getenv" || name == "memchr" || name == "strchr" || name == "strrchr" ||
          name == "strstr" || name == "strpbrk" || name == "getpid" || name == "gettid") return true;
    }
    // Atomic loads/RMWs need an initialized receiver as well as the library
    // contract. Only a defined static-storage atomic (possibly an array
    // element) is modeled here; arbitrary atomic references remain unknown.
    const auto *memberCall = llvm::dyn_cast<clang::CXXMemberCallExpr>(call);
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(callee);
    if (!memberCall || !method || !memoryStandardNamespace(method->getDeclContext())) return false;
    const auto name = method->getNameAsString();
    const auto recordName = method->getParent()->getName();
    if (recordName == "array" && (name == "size" || name == "max_size" || name == "empty")) return true;
    if (recordName == "basic_string_view" && (name == "size" || name == "length" || name == "empty" || name == "data")) {
      const auto *owner = directVariable(memberCall->getImplicitObjectArgument());
      return owner && !owner->getType()->isReferenceType() && !Proofs.Escaped.count(owner) &&
          !Proofs.Mutated.count(owner) && (llvm::isa<clang::ParmVarDecl>(owner) || owner->hasInit());
    }
    if (name != "load" && name != "fetch_sub" && name != "fetch_add" && name != "fetch_or" &&
        name != "fetch_and" && name != "fetch_xor" && name != "exchange") return false;
    if (recordName != "atomic" && recordName != "__atomic_base") return false;
    const clang::Expr *receiver = memberCall->getImplicitObjectArgument()->IgnoreParenImpCasts();
    if (const auto *index = llvm::dyn_cast<clang::CXXOperatorCallExpr>(receiver))
      if (index->getOperator() == clang::OO_Subscript && index->getNumArgs()) receiver = index->getArg(0)->IgnoreParenImpCasts();
    if (const auto *index = llvm::dyn_cast<clang::ArraySubscriptExpr>(receiver)) receiver = index->getBase()->IgnoreParenImpCasts();
    const auto *owner = directVariable(receiver);
    return owner && owner->hasGlobalStorage() && !owner->hasExternalStorage() &&
        !owner->getType()->isReferenceType();
  }
  bool initializedStandardResultMember(const clang::Expr *expression) const {
    if (!expression) return false;
    expression = expression->IgnoreParenImpCasts();
    if (const auto *cast = llvm::dyn_cast<clang::CXXStaticCastExpr>(expression))
      return cast->isPRValue() && cast->getType()->isIntegerType() && initializedStandardResultMember(cast->getSubExpr());
    const clang::Expr *base = nullptr;
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expression)) {
      if (!member->isArrow() && member->getMemberNameInfo().getAsString() == "size") base = member->getBase();
    } else if (const auto *member = llvm::dyn_cast<clang::CXXDependentScopeMemberExpr>(expression)) {
      if (!member->isImplicitAccess() && !member->isArrow() && member->getMemberNameInfo().getAsString() == "size") base = member->getBase();
    }
    const auto *owner = directVariable(base);
    if (!owner || !owner->hasLocalStorage() || owner->getType()->isReferenceType() || !owner->hasInit() ||
        !owner->getType().isConstQualified() || Proofs.Escaped.count(owner) || Proofs.Mutated.count(owner)) return false;
    const clang::Expr *initial = owner->getInit()->IgnoreParenImpCasts();
    if (const auto *cleanup = llvm::dyn_cast<clang::ExprWithCleanups>(initial)) initial = cleanup->getSubExpr()->IgnoreParenImpCasts();
    const auto *call = llvm::dyn_cast<clang::CallExpr>(initial);
    if (!call) return false;
    if (const auto *callee = call->getDirectCallee()) {
      const auto *record = owner->getType()->getAsCXXRecordDecl();
      return record && record->getName() == "format_to_n_result" && memoryStandardNamespace(record->getDeclContext()) &&
          callee->getIdentifier() && callee->getName() == "format_to_n" && memoryStandardNamespace(callee->getDeclContext()) &&
          Context.hasSameUnqualifiedType(callee->getReturnType(), owner->getType());
    }
    // Qualified standard lookup has no ADL additions; every possible selected
    // standard overload guarantees the returned size even before instantiation.
    const auto *lookup = llvm::dyn_cast<clang::UnresolvedLookupExpr>(call->getCallee()->IgnoreParenImpCasts());
    if (!lookup || lookup->requiresADL() || lookup->decls_begin() == lookup->decls_end()) return false;
    for (const auto *decl : lookup->decls()) {
      const auto *templ = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl);
      if (!templ || !templ->getIdentifier() || templ->getName() != "format_to_n" ||
          !memoryStandardNamespace(templ->getDeclContext())) return false;
    }
    return true;
  }
  bool initializedDirectScalar(const clang::Expr *expr, unsigned depth = 0) const {
    if (initializedStandardResultMember(expr)) return true;
    if (depth <= 12 && errnoStorage(expr)) return true;
    if (GlobalInitialization.initialized(expr)) return true;
    const auto *var = directVariable(expr);
    if (boundedRangeIterator(var)) return true;
    if (const auto *parameter = llvm::dyn_cast_or_null<clang::ParmVarDecl>(var)) {
      const auto actual = Proofs.ImmediateLambdaArguments.find(parameter);
      if (depth <= 12 && actual != Proofs.ImmediateLambdaArguments.end() &&
          !Proofs.Mutated.count(var) && !Proofs.Escaped.count(var)) {
        for (const auto *argument : actual->second)
          if (!initializedValue(argument, depth + 1)) return false;
        return !actual->second.empty();
      }
    }
    if (!var || depth > 12 || !var->getType()->isScalarType() || var->getType()->isReferenceType() ||
        (Proofs.Escaped.count(var) && !Proofs.onlyImmediateCapture(var))) return false;
    if (Proofs.Mutated.count(var)) {
      const auto assignments = Proofs.ScalarAssignments.find(var);
      if (Proofs.UnknownScalarMutations.count(var) || assignments == Proofs.ScalarAssignments.end() ||
          (!var->hasInit() && !llvm::isa<clang::ParmVarDecl>(var))) return false;
      for (const auto *value : assignments->second)
        if (!initializedValue(value, depth + 1)) return false;
    }
    if (var->hasInit() && Context.getSourceManager().isPointWithin(
            expr->getExprLoc(), var->getInit()->getBeginLoc(), var->getInit()->getEndLoc()))
      return false; // e.g. int value = value; initialization has not completed.
    if (llvm::isa<clang::ParmVarDecl>(var)) return true;
    if (Proofs.ConstantRangeValues.count(var)) return true;
    if (var->hasGlobalStorage() && !var->getType().isConstQualified()) return false;
    if (!var->hasInit()) return var->hasGlobalStorage() && !var->hasExternalStorage();
    return initializedValue(var->getInit(), depth + 1);
  }
  bool nonLibraryObject(const clang::Expr *expression, unsigned depth) const {
    if (!expression || depth > 8) return false;
    expression = expression->IgnoreParenImpCasts();
    if (const auto *variable = directVariable(expression))
      return !variable->getType()->isReferenceType();
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expression)) {
      const auto *field = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
      if (!field || field->getType()->isReferenceType()) return false;
      return member->isArrow() ? nonLibraryPointer(member->getBase(), depth + 1) : nonLibraryObject(member->getBase(), depth + 1);
    }
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expression))
      if (unary->getOpcode() == clang::UO_Deref) return nonLibraryPointer(unary->getSubExpr(), depth + 1);
    return false;
  }
  bool nonLibraryPointer(const clang::Expr *expr, unsigned depth = 0) const {
    // Dependent initializers such as ParenListExpr can have a null QualType;
    // they are unresolved provenance, not a typed pointer expression.
    if (!expr || depth > 8 || expr->getType().isNull() || expr->isTypeDependent()) return false;
    if (expr->getType()->isFunctionPointerType() || expr->getType()->isMemberPointerType()) return true;
    if (baseline_library::Proof(Context).validStream(expr)) return true;
    expr = expr->IgnoreParenImpCasts();
    if (llvm::isa<clang::CXXThisExpr>(expr) || llvm::isa<clang::StringLiteral>(expr) ||
        llvm::isa<clang::PredefinedExpr>(expr) || llvm::isa<clang::CXXNullPtrLiteralExpr>(expr)) return true;
    // The implementation's errno accessor returns the calling thread's
    // storage; other library calls alter its value, not its address/lifetime.
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(expr))
      if (const auto *function = call->getDirectCallee())
        if (function->getIdentifier() && function->getName() == "__errno_location" && function->isExternC() &&
            Context.getSourceManager().isInSystemHeader(function->getLocation())) return true;
    if (auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expr))
      if (unary->getOpcode() == clang::UO_AddrOf)
        if (const auto *var = directVariable(unary->getSubExpr()))
          if (!var->getType()->isReferenceType()) return true;
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
      const auto *field = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
      if (field && field->getType()->isArrayType() && nonLibraryObject(member, depth + 1)) return true;
    }
    if (const auto *var = directVariable(expr)) {
      if (Proofs.BoundedRangeIterators.count(var)) return true;
      if (const auto *parameter = llvm::dyn_cast<clang::ParmVarDecl>(var)) {
        const auto argument = Proofs.ImmediateLambdaArguments.find(parameter);
        if (argument != Proofs.ImmediateLambdaArguments.end() && !Proofs.Mutated.count(var) && !Proofs.Escaped.count(var)) {
          for (const auto *actual : argument->second)
            if (!nonLibraryPointer(actual, depth + 1)) return false;
          return !argument->second.empty();
        }
      }
      if (var->getType()->isArrayType()) return true;
      if (var->hasInit() && (!var->hasGlobalStorage() || var->getType().isConstQualified()) &&
          !Proofs.Mutated.count(var) && !Proofs.Escaped.count(var))
        return nonLibraryPointer(var->getInit(), depth + 1);
    }
    return false;
  }
  bool disjointArrayCopy(const clang::CallExpr *expr) const {
    if (expr->getNumArgs() != 3) return false;
    const auto *callee = expr->getDirectCallee();
    if (!callee || (callee->getName() != "memcpy" && callee->getName() != "memmove" &&
                    callee->getName() != "__builtin_memcpy" && callee->getName() != "__builtin_memmove")) return false;
    const auto *destination = directVariable(expr->getArg(0));
    const auto *source = directVariable(expr->getArg(1));
    if (!destination || !source || destination == source ||
        !Context.getAsConstantArrayType(destination->getType()) ||
        !Context.getAsConstantArrayType(source->getType())) return false;
    clang::Expr::EvalResult length;
    if (expr->getArg(2)->isValueDependent() || !expr->getArg(2)->EvaluateAsInt(length, Context)) return false;
    const auto &bytes = length.Val.getInt();
    return !bytes.isNegative() &&
        bytes.getLimitedValue() <= static_cast<uint64_t>(Context.getTypeSizeInChars(destination->getType()).getQuantity()) &&
        bytes.getLimitedValue() <= static_cast<uint64_t>(Context.getTypeSizeInChars(source->getType()).getQuantity());
  }
  bool boundedArrayIndex(const clang::ArraySubscriptExpr *expr) const {
    const auto *array = Context.getAsConstantArrayType(expr->getBase()->IgnoreParenImpCasts()->getType());
    if (!array || expr->getIdx()->isValueDependent()) return false;
    clang::Expr::EvalResult value;
    if (!expr->getIdx()->EvaluateAsInt(value, Context)) return false;
    const auto &index = value.Val.getInt();
    return !index.isNegative() && index.getLimitedValue() < array->getSize().getLimitedValue();
  }
  const clang::VarDecl *ownedStorageRoot(const clang::Expr *expression, unsigned depth = 0) const {
    if (!expression || depth > 10) return nullptr;
    expression = expression->IgnoreParenImpCasts();
    if (const auto *variable = directVariable(expression)) {
      if (!variable->getType()->isReferenceType()) return variable->getCanonicalDecl();
      return nullptr;
    }
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expression)) {
      const auto *field = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
      if (!field || member->isArrow() || field->getType()->isReferenceType() || field->getParent()->isUnion()) return nullptr;
      return ownedStorageRoot(member->getBase(), depth + 1);
    }
    if (const auto *index = llvm::dyn_cast<clang::ArraySubscriptExpr>(expression)) {
      if (!boundedArrayIndex(index)) return nullptr;
      return ownedStorageRoot(index->getBase(), depth + 1);
    }
    return nullptr;
  }
  bool safeScalarAssignment(const clang::BinaryOperator *expr) const {
    if (!expr->getLHS()->getType()->isScalarType()) return false;
    const auto *rhs = expr->getRHS()->IgnoreParenImpCasts();
    // Literal/computed values have no overlapping source object. Retain
    // lvalue-source copies through pointers/references, where overlap matters.
    if (rhs->isPRValue() && !llvm::isa<clang::CXXConstructExpr>(rhs)) return true;
    const auto *leftRoot = ownedStorageRoot(expr->getLHS());
    const auto *rightRoot = ownedStorageRoot(rhs);
    if (leftRoot && rightRoot && leftRoot != rightRoot) return true;
    const auto *left = directVariable(expr->getLHS());
    const auto *right = directVariable(rhs);
    if (left && right && !left->getType()->isReferenceType() && !right->getType()->isReferenceType()) return true;
    return liveDirectObject(expr->getLHS());
  }
  void unresolvedEffects(clang::SourceLocation loc, bool assembly = false) {
    // These are coverage obligations, never fabricated violation verdicts.
    // Even a call with no arguments may manipulate global/captured state.
    const struct { const char *rule; const char *reason; } gaps[] = {
        {"6.8.1", "callee-lifetime-effects-unresolved"},
        {"11.6.2", "callee-initialization-effects-unresolved"},
        {"8.18.1", "callee-copy-effects-unresolved"},
        {"25.5.3", "callee-library-pointer-invalidation-unresolved"},
        {"30.0.2", "callee-stream-transition-effects-unresolved"},
        {"8.7.1", "callee-pointer-arithmetic-unresolved"},
        {"8.7.2", "callee-pointer-subtraction-unresolved"},
        {"8.9.1", "callee-pointer-order-unresolved"},
    };
    for (const auto &gap : gaps)
      emitBaselineCandidate(Context, gap.rule, loc, assembly ? "inline-assembly-memory-effects-unresolved" : gap.reason, true);
  }

  bool implicitTrivialCopyBody(const clang::Expr *expression) const {
    clang::DynTypedNode node = clang::DynTypedNode::create(*expression);
    for (unsigned depth = 0; depth != 32; ++depth) {
      const auto parents = Context.getParents(node);
      if (parents.size() != 1) return false;
      const auto &parent = parents[0];
      if (const auto *method = parent.get<clang::CXXMethodDecl>()) {
        const auto *constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(method);
        return method->isImplicit() && method->isDefaulted() && method->isTrivial() &&
            (method->isCopyAssignmentOperator() || method->isMoveAssignmentOperator() ||
             (constructor && constructor->isCopyOrMoveConstructor()));
      }
      if (parent.get<clang::FunctionDecl>()) return false;
      node = parent;
    }
    return false;
  }
  void emit(llvm::StringRef rule, const clang::Expr *expr, llvm::StringRef reason, bool gap = false) {
    // Compiler-generated trivial copying cannot itself end either object's
    // lifetime. Its live-object entry contract is checked at each caller,
    // including an explicit candidate for copy/move assignment calls above.
    if (rule == "6.8.1" && !gap && implicitTrivialCopyBody(expr)) return;
    emitBaselineCandidate(Context, rule, expr->getExprLoc(), reason, gap);
  }
  clang::ASTContext &Context;
  const MemoryProofInputs &Proofs;
  mutable baseline_initialization::Proof GlobalInitialization;
};
}  // namespace

void collectMemoryBaselineFacts(clang::ASTContext &context) {
  MemoryProofInputs proofs(context);
  proofs.TraverseDecl(context.getTranslationUnitDecl());
  MemoryBaselineVisitor(context, proofs).TraverseDecl(context.getTranslationUnitDecl());
}
