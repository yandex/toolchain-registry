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
        const CXXMethodDecl *VirtualMethod = nullptr;
        if (const auto *Call = dyn_cast<CXXMemberCallExpr>(E)) {
            const auto *M = Call->getMethodDecl();
            const auto *Member = dyn_cast<MemberExpr>(Call->getCallee()->IgnoreParenImpCasts());
            Violation = M && M->isVirtual() && Member && !Member->hasQualifier() &&
                currentObject(Call->getImplicitObjectArgument());
            if (Violation) VirtualMethod = M;
        } else if (const auto *T = dyn_cast<CXXTypeidExpr>(E)) {
            Violation = !T->isTypeOperand() && T->isPotentiallyEvaluated() &&
                currentObject(T->getExprOperand());
        } else if (const auto *D = dyn_cast<CXXDynamicCastExpr>(E)) {
            Violation = currentObject(D->getSubExpr());
        }
        if (!Violation || !isInAnalyzedCode(*E, E->getExprLoc(), C)) return;
        const unsigned Destruction = isa<CXXDestructorDecl>(F);
        for (const Decl *Instance : Instances.claim(*E, E->getExprLoc(), C)) {
            if (VirtualMethod) {
                diagnoseAnalysisInstance(*this, Instance, C, E->getExprLoc(),
                    "virtual call to %0 on the current object during %select{construction|destruction}1",
                    VirtualMethod, Destruction);
                diag(VirtualMethod->getLocation(), "virtual function %0 declared here",
                     DiagnosticIDs::Note) << VirtualMethod;
            } else if (const auto *T = dyn_cast<CXXTypeidExpr>(E)) {
                diagnoseAnalysisInstance(*this, Instance, C, E->getExprLoc(),
                    "typeid uses the current object's polymorphic type %0 during %select{construction|destruction}1",
                    T->getExprOperand()->getType(), Destruction);
            } else if (const auto *D = dyn_cast<CXXDynamicCastExpr>(E)) {
                diagnoseAnalysisInstance(*this, Instance, C, E->getExprLoc(),
                    "dynamic_cast from %0 to %1 uses the current object during %select{construction|destruction}2",
                    D->getSubExpr()->getType(), D->getTypeAsWritten(), Destruction);
            }
            diag(F->getLocation(), "in %select{constructor|destructor}0 %1",
                 DiagnosticIDs::Note) << Destruction << F;
        }
    };
    if (const auto *Ctor = dyn_cast<CXXConstructorDecl>(F))
        for (const auto *Init : Ctor->inits()) local_evidence::walk(Init->getInit(), C, Inspect, true);
    local_evidence::walk(F->getBody(), C, Inspect, true);
}
} // namespace clang::tidy::sdc
