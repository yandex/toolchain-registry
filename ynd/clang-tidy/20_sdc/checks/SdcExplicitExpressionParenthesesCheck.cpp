#include "SdcExplicitExpressionParenthesesCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "SdcPreprocessorExpressions.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
namespace {
int rank(BinaryOperatorKind O) {
    switch (O) {
    case BO_Mul: case BO_Div: case BO_Rem: return 13;
    case BO_Add: case BO_Sub: return 12;
    case BO_Shl: case BO_Shr: return 11;
    case BO_LT: case BO_GT: case BO_LE: case BO_GE: return 10;
    case BO_EQ: case BO_NE: return 9;
    case BO_And: return 8;
    case BO_Xor: return 7;
    case BO_Or: return 6;
    case BO_LAnd: return 5;
    case BO_LOr: return 4;
    case BO_Comma: return 0;
    default: return 2;
    }
}
int expressionRank(const Expr *E) {
    E = E->IgnoreImpCasts();
    if (const auto *B = dyn_cast<BinaryOperator>(E)) return rank(B->getOpcode());
    if (const auto *O = dyn_cast<CXXOperatorCallExpr>(E)) {
        if (O->getNumArgs() == 2 && O->isInfixBinaryOp())
            return rank(BinaryOperator::getOverloadedOpcode(O->getOperator()));
    }
    if (isa<AbstractConditionalOperator>(E)) return 3;
    if (isa<CXXThrowExpr>(E)) return 1;
    return 14;
}
} // namespace
void SdcExplicitExpressionParenthesesCheck::registerPPCallbacks(const SourceManager &, Preprocessor *PP, Preprocessor *) {
    registerExpressionPreprocessing(*this, *PP, true);
}
void SdcExplicitExpressionParenthesesCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(expr().bind("expr"), this);
}
void SdcExplicitExpressionParenthesesCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *E = Result.Nodes.getNodeAs<Expr>("expr");
    if (!E) return;
    SourceLocation L = E->getExprLoc();
    auto N = DynTypedNode::create(*E);
    if (!isInAnalyzedCode(N, L, C)) return;
    const auto *B = dyn_cast_or_null<BinaryOperator>(E);
    const auto *O = dyn_cast_or_null<CXXOperatorCallExpr>(E);
    StringRef Message;
    int Rank = expressionRank(E);
    bool Bad = false;
    auto Operand = [&](const Expr *A) {
        int AR = expressionRank(A);
        Bad |= AR != 14 && AR > Rank;
    };
    if (Rank >= 3 && Rank <= 13) {
        if (B) { Operand(B->getLHS()); Operand(B->getRHS()); }
        if (O) for (const auto *A : O->arguments()) Operand(A);
        if (const auto *Q = dyn_cast<ConditionalOperator>(E)) {
            Operand(Q->getCond()); Operand(Q->getTrueExpr()); Operand(Q->getFalseExpr());
        }
    }
    if (const auto *S = dyn_cast<UnaryExprOrTypeTraitExpr>(E))
        if (S->getKind() == UETT_SizeOf && !S->isArgumentType())
            Bad |= !isa<ParenExpr>(S->getArgumentExpr());
    if (Bad) Message = "parenthesize operands to make expression grouping explicit";
    if (!Message.empty())
        emitPolicyDiagnostic(*this, N, L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
