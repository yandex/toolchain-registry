#include "SdcShortCircuitSideEffectsCheck.h"
#include "SdcLocalEvidenceUtils.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
void SdcShortCircuitSideEffectsCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(binaryOperator(anyOf(hasOperatorName("&&"), hasOperatorName("||")))
                       .bind("logical"), this);
}
void SdcShortCircuitSideEffectsCheck::check(const MatchFinder::MatchResult &Result) {
    const auto *B = Result.Nodes.getNodeAs<BinaryOperator>("logical");
    auto &C = *Result.Context;
    if (!B || !isInAnalyzedCode(*B, B->getOperatorLoc(), C) ||
        !local_evidence::visibleEvaluation(B, C)) return;
    bool V;
    if (local_evidence::constantBool(B->getLHS(), C, V) &&
        ((B->getOpcode() == BO_LAnd && !V) || (B->getOpcode() == BO_LOr && V))) return;
    const Expr *Evidence = nullptr;
    const ValueDecl *Object = nullptr;
    bool VolatileRead = false;
    local_evidence::walk(B->getRHS(), C, [&](const Stmt *S) {
        if (Evidence) return;
        if (const auto *A = dyn_cast<BinaryOperator>(S)) {
            if (A->isAssignmentOp() && local_evidence::directObject(A->getLHS())) {
                Evidence = A;
                Object = local_evidence::directObject(A->getLHS());
            }
        } else if (const auto *U = dyn_cast<UnaryOperator>(S)) {
            if (U->isIncrementDecrementOp() && local_evidence::directObject(U->getSubExpr())) {
                Evidence = U;
                Object = local_evidence::directObject(U->getSubExpr());
            }
        } else if (const auto *I = dyn_cast<ImplicitCastExpr>(S)) {
            if (I->getCastKind() == CK_LValueToRValue &&
                I->getSubExpr()->getType().isVolatileQualified()) {
                Evidence = I;
                VolatileRead = true;
                const auto *Operand = I->getSubExpr()->IgnoreParenImpCasts();
                if (const auto *R = dyn_cast<DeclRefExpr>(Operand)) Object = R->getDecl();
                else if (const auto *M = dyn_cast<MemberExpr>(Operand)) Object = M->getMemberDecl();
            }
        }
    }, true);
    // Never infer effects from a function's name, exception specification,
    // missing body, or the mere presence of a call.
    if (!Evidence || !isWrittenInAnalyzedSource(Evidence->getExprLoc(), *Result.SourceManager)) return;
    for (const Decl *Instance : Instances.claim(*B, B->getOperatorLoc(), C)) {
        diagnoseAnalysisInstance(*this, Instance, C, B->getOperatorLoc(),
            "right-hand operand has a visible persistent side effect");
        if (VolatileRead) {
            const QualType Type = cast<ImplicitCastExpr>(Evidence)->getSubExpr()->getType();
            if (Object)
                diag(Evidence->getExprLoc(), "volatile-qualified object %0 of type %1 is read here",
                     DiagnosticIDs::Note) << Object << Type;
            else
                diag(Evidence->getExprLoc(), "volatile-qualified value of type %0 is read here",
                     DiagnosticIDs::Note) << Type;
            if (Object && Object->getLocation().isValid())
                diag(Object->getLocation(), "object %0 declared here with type %1",
                     DiagnosticIDs::Note) << Object << Object->getType();
        } else {
            diag(Evidence->getExprLoc(), "object %0 of type %1 is modified here",
                 DiagnosticIDs::Note) << Object << Object->getType();
        }
    }
}
} // namespace clang::tidy::sdc
