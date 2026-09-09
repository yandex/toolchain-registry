#include "SdcNoCommaOperatorCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/ParentMapContext.h"
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
    auto N = DynTypedNode::create(*E);
    for (unsigned I = 0; I < 64; ++I) {
        auto P = C.getParents(N); if (P.empty()) break;
        if (P[0].get<CXXFoldExpr>()) return true;
        if (P[0].get<Decl>() || P[0].get<CompoundStmt>()) break;
        N = P[0];
    }
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
    StringRef Message;
    if (((B && B->getOpcode() == BO_Comma) || (O && O->getOperator() == OO_Comma)) && !foldedComma(E, C))
        Message = "do not use the comma operator outside a fold expression";
    if (!Message.empty())
        emitPolicyDiagnostic(*this, N, L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
