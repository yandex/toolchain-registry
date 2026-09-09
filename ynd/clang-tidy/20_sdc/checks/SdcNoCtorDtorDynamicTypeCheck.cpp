#include "SdcNoCtorDtorDynamicTypeCheck.h"
#include "SdcLocalEvidenceUtils.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
namespace {
bool currentObject(const Expr *E) {
    if (!E) return false;
    E = E->IgnoreParenImpCasts();
    if (isa<CXXThisExpr>(E)) return true;
    const auto *U = dyn_cast<UnaryOperator>(E);
    return U && U->getOpcode() == UO_Deref &&
        isa<CXXThisExpr>(U->getSubExpr()->IgnoreParenImpCasts());
}
} // namespace
void SdcNoCtorDtorDynamicTypeCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(functionDecl(anyOf(cxxConstructorDecl(), cxxDestructorDecl()),
                                   isDefinition()).bind("function"), this);
}
void SdcNoCtorDtorDynamicTypeCheck::check(const MatchFinder::MatchResult &Result) {
    const auto *F = Result.Nodes.getNodeAs<FunctionDecl>("function");
    auto &C = *Result.Context;
    if (!F || F->isInvalidDecl() || !isInMaterializedCode(*F, C)) return;
    auto Inspect = [&](const Stmt *S) {
        const auto *E = dyn_cast<Expr>(S);
        if (!E) return;
        bool Violation = false;
        if (const auto *Call = dyn_cast<CXXMemberCallExpr>(E)) {
            const auto *M = Call->getMethodDecl();
            const auto *Member = dyn_cast<MemberExpr>(Call->getCallee()->IgnoreParenImpCasts());
            Violation = M && M->isVirtual() && Member && !Member->hasQualifier() &&
                currentObject(Call->getImplicitObjectArgument());
        } else if (const auto *T = dyn_cast<CXXTypeidExpr>(E)) {
            Violation = !T->isTypeOperand() && T->isPotentiallyEvaluated() &&
                currentObject(T->getExprOperand());
        } else if (const auto *D = dyn_cast<CXXDynamicCastExpr>(E)) {
            Violation = currentObject(D->getSubExpr());
        }
        if (Violation && isInAnalyzedCode(*E, E->getExprLoc(), C))
            emitPolicyDiagnostic(*this, DynTypedNode::create(*E), E->getExprLoc(),
                "do not use the current object's dynamic type during construction or destruction",
                C, Instances);
    };
    if (const auto *Ctor = dyn_cast<CXXConstructorDecl>(F))
        for (const auto *Init : Ctor->inits()) local_evidence::walk(Init->getInit(), C, Inspect, true);
    local_evidence::walk(F->getBody(), C, Inspect, true);
}
} // namespace clang::tidy::sdc
