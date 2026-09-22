#include "SdcNoCommaOperatorCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
namespace {
bool foldedComma(const Expr *E, ASTContext &C) {
    // Instantiation lowers folds to binary operators located at the ellipsis.
    // A written comma has spelling ','; the lowering is not a written use.
    Token T;
    auto L = C.getSourceManager().getSpellingLoc(E->getExprLoc());
    if (!Lexer::getRawToken(L, T, C.getSourceManager(), C.getLangOpts(), true) && T.is(tok::ellipsis)) return true;
    // A written comma inside a fold operand is still prohibited. Only the
    // fold operator itself (lowered at the ellipsis) receives the exception.
    return false;
}

} // namespace
void SdcNoCommaOperatorCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(expr().bind("expr"), this);
}
void SdcNoCommaOperatorCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *E = Result.Nodes.getNodeAs<Expr>("expr");
    if (!E) return;
    SourceLocation L = E->getExprLoc();
    auto N = DynTypedNode::create(*E);
    if (!isInAnalyzedCode(N, L, C)) return;
    const auto *B = dyn_cast_or_null<BinaryOperator>(E);
    const auto *O = dyn_cast_or_null<CXXOperatorCallExpr>(E);
    if (!((B && B->getOpcode() == BO_Comma) ||
          (O && O->getOperator() == OO_Comma)) || foldedComma(E, C)) return;
    for (const Decl *Instance : Instances.claim(N, L, C)) {
        if (O) {
            const auto *Callee = O->getDirectCallee();
            diagnoseAnalysisInstance(*this, Instance, C, L,
                "do not use overloaded comma operator '%0' with operand types %1 "
                "and %2; only the fold operator is exempt",
                Callee ? Callee->getQualifiedNameAsString() : "operator,",
                policyTypeName(O->getArg(0)->getType(), C),
                policyTypeName(O->getArg(1)->getType(), C));
        } else {
            diagnoseAnalysisInstance(*this, Instance, C, L,
                "do not use the built-in comma operator; only the fold operator is exempt");
        }
    }
}
} // namespace clang::tidy::sdc
