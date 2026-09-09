#include "SdcNoConstantUnsignedWrapCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "SdcExpressionUtils.h"
#include "SdcPreprocessorExpressions.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Analysis/Analyses/ExprMutationAnalyzer.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
using namespace expression_detail;
namespace {
bool unevaluatedBranch(const Expr *E, ASTContext &C) {
    if (ExprMutationAnalyzer::isUnevaluated(E, C)) return true;
    auto N = DynTypedNode::create(*E);
    for (unsigned I = 0; I < 128; ++I) {
        auto P = C.getParents(N);
        if (P.empty()) break;
        const auto *Child = N.get<Expr>();
        llvm::APSInt V;
        if (const auto *B = P[0].get<BinaryOperator>())
            if (Child == B->getRHS() && constant(B->getLHS(), C, V) &&
                ((B->getOpcode() == BO_LAnd && V == 0) ||
                 (B->getOpcode() == BO_LOr && V != 0))) return true;
        if (const auto *Q = P[0].get<ConditionalOperator>())
            if (constant(Q->getCond(), C, V) &&
                ((Child == Q->getTrueExpr() && V == 0) ||
                 (Child == Q->getFalseExpr() && V != 0))) return true;
        if (P[0].get<FunctionDecl>() || P[0].get<LambdaExpr>()) break;
        N = P[0];
    }
    return false;
}
} // namespace
void SdcNoConstantUnsignedWrapCheck::registerPPCallbacks(const SourceManager &, Preprocessor *PP, Preprocessor *) {
    registerExpressionPreprocessing(*this, *PP, false);
}
void SdcNoConstantUnsignedWrapCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(expr().bind("expr"), this);
}
void SdcNoConstantUnsignedWrapCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *E = Result.Nodes.getNodeAs<Expr>("expr");
    if (!E) return;
    SourceLocation L = E->getExprLoc();
    auto N = DynTypedNode::create(*E);
    if (!isInAnalyzedCode(N, L, C)) return;
    const auto *B = dyn_cast_or_null<BinaryOperator>(E);
    StringRef Message;
    if (E->getType()->isUnsignedIntegerType() && !unevaluatedBranch(E, C)) {
        bool Wrap = false;
        llvm::APSInt Left, Right;
        if (B && constant(B->getLHS(), C, Left) && constant(B->getRHS(), C, Right)) {
            unsigned Width = C.getIntWidth(E->getType());
            llvm::APInt A = Left.extOrTrunc(Width), R = Right.extOrTrunc(Width);
            switch (B->getOpcode()) {
            case BO_Add: (void)A.uadd_ov(R, Wrap); break;
            case BO_Sub: (void)A.usub_ov(R, Wrap); break;
            case BO_Mul: (void)A.umul_ov(R, Wrap); break;
            case BO_Shl:
                if (R.getLimitedValue() < Width)
                    Wrap = A.getActiveBits() + R.getLimitedValue() > Width;
                break;
            default: break;
            }
        }
        if (const auto *U = dyn_cast<UnaryOperator>(E))
            if (U->getOpcode() == UO_Minus && constant(U->getSubExpr(), C, Left)) Wrap = Left != 0;
        if (Wrap) Message = "unsigned arithmetic with constant operands should not wrap";
    }
    if (!Message.empty())
        emitPolicyDiagnostic(*this, N, L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
