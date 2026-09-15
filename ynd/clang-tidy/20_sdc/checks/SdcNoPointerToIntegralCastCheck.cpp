#include "SdcNoPointerToIntegralCastCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
void SdcNoPointerToIntegralCastCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(expr().bind("expr"), this);
}
void SdcNoPointerToIntegralCastCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *E = Result.Nodes.getNodeAs<Expr>("expr");
    if (!E) return;
    SourceLocation L = E->getExprLoc();
    auto N = DynTypedNode::create(*E);
    if (!isInAnalyzedCode(N, L, C)) return;
    StringRef Message;
    const auto *Cast = dyn_cast<ExplicitCastExpr>(E);
    if (Cast)
        if (Cast->getSubExpr()->IgnoreParenImpCasts()->getType()->isPointerType() && Cast->getType()->isIntegralOrEnumerationType())
            Message = "do not cast pointer type %0 to integral type %1";
    if (!Message.empty())
        for (const Decl *Instance : Instances.claim(N, L, C))
            diagnoseAnalysisInstance(*this, Instance, C, L, Message,
                                     Cast->getSubExpr()->IgnoreParenImpCasts()->getType(),
                                     Cast->getTypeAsWritten());
}
} // namespace clang::tidy::sdc
