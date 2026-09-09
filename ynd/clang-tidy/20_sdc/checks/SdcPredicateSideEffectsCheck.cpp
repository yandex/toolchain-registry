#include "SdcPredicateSideEffectsCheck.h"
#include "SdcLocalEvidenceUtils.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include <set>

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
namespace {
bool standardEntity(const NamedDecl *D) {
    const DeclContext *DC = D->getDeclContext();
    while (const auto *NS = dyn_cast<NamespaceDecl>(DC)) {
        if (NS->isStdNamespace()) return true;
        if (!NS->isInline()) return false;
        DC = NS->getParent();
    }
    return false;
}
// Only the ordinary, non-execution-policy overloads of these algorithms are
// modelled. In particular, ranges projections are not guessed to be predicates.
int predicateIndex(const FunctionDecl *F, unsigned Arity) {
    if (!F || !F->getIdentifier() || !standardEntity(F)) return -1;
    StringRef Name = F->getName();
    if (Arity == 3 && (Name == "all_of" || Name == "any_of" || Name == "none_of" ||
                      Name == "find_if" || Name == "find_if_not" || Name == "count_if" ||
                      Name == "remove_if" || Name == "sort" || Name == "stable_sort" ||
                      Name == "is_sorted" || Name == "is_sorted_until" || Name == "unique" ||
                      Name == "partition" || Name == "stable_partition")) return 2;
    if (Arity == 4 && (Name == "lower_bound" || Name == "upper_bound" ||
                      Name == "equal_range" || Name == "binary_search" ||
                      Name == "remove_copy_if")) return 3;
    return -1;
}
const CXXMethodDecl *singleCallOperator(const CXXRecordDecl *R) {
    if (!R || !(R = R->getDefinition())) return nullptr;
    const CXXMethodDecl *Found = nullptr;
    for (const auto *M : R->methods()) {
        if (!M->isOverloadedOperator() || M->getOverloadedOperator() != OO_Call) continue;
        if (Found && Found->getCanonicalDecl() != M->getCanonicalDecl()) return nullptr;
        Found = M;
    }
    return Found;
}
const VarDecl *referencedVariable(const Expr *E) {
    const auto *R = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
    return R ? dyn_cast<VarDecl>(R->getDecl()) : nullptr;
}
const Expr *bodyEffect(const FunctionDecl *F, const LambdaExpr *L, ASTContext &C) {
    if (!F || !F->hasBody()) return nullptr;
    std::set<const Decl *> References;
    if (L) for (const auto &Capture : L->captures())
        if (Capture.capturesVariable() && Capture.getCaptureKind() == LCK_ByRef)
            References.insert(Capture.getCapturedVar()->getCanonicalDecl());
    auto Persistent = [&](const Expr *Target) {
        const auto *V = referencedVariable(Target);
        if (!V) return false;
        if (V->hasGlobalStorage() || References.count(V->getCanonicalDecl())) return true;
        return isa<ParmVarDecl>(V) && V->getType()->isReferenceType() &&
            V->getDeclContext() == F;
    };
    const Expr *Evidence = nullptr;
    local_evidence::walk(F->getBody(), C, [&](const Stmt *S) {
        if (Evidence) return;
        if (const auto *B = dyn_cast<BinaryOperator>(S)) {
            if (B->isAssignmentOp() && Persistent(B->getLHS())) Evidence = B;
        } else if (const auto *U = dyn_cast<UnaryOperator>(S)) {
            if (U->isIncrementDecrementOp() && Persistent(U->getSubExpr())) Evidence = U;
        } else if (const auto *I = dyn_cast<ImplicitCastExpr>(S)) {
            if (I->getCastKind() == CK_LValueToRValue &&
                I->getSubExpr()->getType().isVolatileQualified()) Evidence = I;
        }
    }, true);
    return Evidence;
}
} // namespace
void SdcPredicateSideEffectsCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(callExpr().bind("call"), this);
    Finder->addMatcher(varDecl(unless(isImplicit())).bind("variable"), this);
}
void SdcPredicateSideEffectsCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *Call = Result.Nodes.getNodeAs<CallExpr>("call");
    const auto *Variable = Result.Nodes.getNodeAs<VarDecl>("variable");
    const FunctionDecl *Function = nullptr;
    const CXXMethodDecl *Operator = nullptr;
    const LambdaExpr *Lambda = nullptr;
    SourceLocation Location;
    DynTypedNode Node;
    if (Call) {
        if (!isInAnalyzedCode(*Call, Call->getExprLoc(), C)) return;
        int Index = predicateIndex(Call->getDirectCallee(), Call->getNumArgs());
        if (Index < 0 || !local_evidence::visibleEvaluation(Call, C)) return;
        const auto *Primary = Call->getDirectCallee()->getPrimaryTemplate();
        if (!Primary || Primary->getTemplatedDecl()->getNumParams() <= static_cast<unsigned>(Index)) return;
        QualType Role = Primary->getTemplatedDecl()->getParamDecl(Index)->getType().getNonReferenceType();
        const auto *Parameter = Role->getAs<TemplateTypeParmType>();
        if (!Parameter || Parameter->getDepth() != 0 ||
            Parameter->getIndex() >= Primary->getTemplateParameters()->size()) return;
        // Canonical parameter types can omit the declaration pointer. Recover
        // the declaration by its depth/index in this primary template.
        const auto *RoleDecl = dyn_cast<TemplateTypeParmDecl>(
            Primary->getTemplateParameters()->getParam(Parameter->getIndex()));
        if (!RoleDecl) return;
        StringRef RoleName = RoleDecl->getName().ltrim("_");
        if (RoleName != "Compare" && RoleName != "Predicate" && RoleName != "BinaryPredicate") return;
        const Expr *Argument = Call->getArg(Index)->IgnoreParenImpCasts();
        Location = Argument->getExprLoc(); Node = DynTypedNode::create(*Call);
        if (const auto *Clean = dyn_cast<ExprWithCleanups>(Argument)) Argument = Clean->getSubExpr()->IgnoreParenImpCasts();
        if (const auto *Materialized = dyn_cast<MaterializeTemporaryExpr>(Argument)) Argument = Materialized->getSubExpr()->IgnoreParenImpCasts();
        if (const auto *Bound = dyn_cast<CXXBindTemporaryExpr>(Argument)) Argument = Bound->getSubExpr()->IgnoreParenImpCasts();
        Lambda = dyn_cast<LambdaExpr>(Argument);
        if (Lambda) Operator = Lambda->getCallOperator();
        else if (const auto *R = dyn_cast<DeclRefExpr>(Argument)) {
            Function = dyn_cast<FunctionDecl>(R->getDecl());
            // A named lambda can have changed since initialization. Its type
            // still proves constness, but captures/body are not reconstructed.
        }
        if (!Function && !Operator) Operator = singleCallOperator(Argument->getType()->getAsCXXRecordDecl());
    } else if (Variable) {
        if (!isInAnalyzedCode(*Variable, Variable->getLocation(), C)) return;
        if (Variable->getType()->isReferenceType() || Variable->getType()->isPointerType()) return;
        const auto *S = dyn_cast_or_null<ClassTemplateSpecializationDecl>(Variable->getType()->getAsCXXRecordDecl());
        if (!S || !standardEntity(S)) return;
        StringRef Name = S->getName();
        unsigned Index;
        if (Name == "set" || Name == "multiset") Index = 1;
        else if (Name == "map" || Name == "multimap" || Name == "priority_queue") Index = 2;
        else return;
        if (S->getTemplateArgs().size() <= Index) return;
        const auto &A = S->getTemplateArgs()[Index];
        if (A.getKind() != TemplateArgument::Type) return;
        Operator = singleCallOperator(A.getAsType()->getAsCXXRecordDecl());
        Location = Variable->getLocation(); Node = DynTypedNode::create(*Variable);
    } else return;
    if (!isInAnalyzedCode(Node, Location, C)) return;
    if (Operator) Function = Operator;
    if (!Function || Function->isInvalidDecl()) return;
    bool NonConst = Operator && !Operator->isConst() && !Operator->isStatic();
    const Expr *Effect = NonConst ? nullptr : bodyEffect(Function, Lambda, C);
    if (!NonConst && !Effect) return;
    for (const Decl *Instance : Instances.claim(Node, Location, C)) {
        diagnoseAnalysisInstance(*this, Instance, C, Location, NonConst ?
            "predicate call operator must be const" : "predicate has a visible persistent side effect");
        diag(NonConst ? Operator->getLocation() : Effect->getExprLoc(),
             NonConst ? "non-const call operator is declared here" : "persistent side effect occurs here",
             DiagnosticIDs::Note);
    }
}
} // namespace clang::tidy::sdc
