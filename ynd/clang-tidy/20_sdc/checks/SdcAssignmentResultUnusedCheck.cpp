#include "SdcAssignmentResultUnusedCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "SdcExpressionUtils.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Analysis/Analyses/ExprMutationAnalyzer.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
using namespace expression_detail;
namespace {
bool assignment(OverloadedOperatorKind O) {
    switch (O) {
    case OO_Equal: case OO_PlusEqual: case OO_MinusEqual: case OO_StarEqual:
    case OO_SlashEqual: case OO_PercentEqual: case OO_CaretEqual: case OO_AmpEqual:
    case OO_PipeEqual: case OO_LessLessEqual: case OO_GreaterGreaterEqual: return true;
    default: return false;
    }
}
bool resultUsed(const Expr *E, ASTContext &C) {
    const auto P = semanticParent(E, C);
    if (const auto *Cast = P.get<CastExpr>()) return !Cast->getType()->isVoidType();
    if (const auto *B = P.get<BinaryOperator>()) {
        if (B->getOpcode() == BO_Comma) {
            if (peel(B->getLHS()) == E) return false;
            return resultUsed(B, C);
        }
        return true;
    }
    if (const auto *F = P.get<ForStmt>()) return peel(F->getCond()) == E;
    if (const auto *I = P.get<IfStmt>()) return peel(I->getCond()) == E;
    if (const auto *W = P.get<WhileStmt>()) return peel(W->getCond()) == E;
    if (const auto *D = P.get<DoStmt>()) return peel(D->getCond()) == E;
    if (const auto *S = P.get<SwitchStmt>()) return peel(S->getCond()) == E;
    return P.get<Expr>() || P.get<VarDecl>() || P.get<FieldDecl>() || P.get<ReturnStmt>();
}
} // namespace
void SdcAssignmentResultUnusedCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(expr().bind("expr"), this);
}
void SdcAssignmentResultUnusedCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *E = Result.Nodes.getNodeAs<Expr>("expr");
    if (!E) return;
    SourceLocation L = E->getExprLoc();
    auto N = DynTypedNode::create(*E);
    if (!isInAnalyzedCode(N, L, C)) return;
    const auto *B = dyn_cast_or_null<BinaryOperator>(E);
    const auto *O = dyn_cast_or_null<CXXOperatorCallExpr>(E);
    StringRef Message;
    if (((B && B->isAssignmentOp()) || (O && assignment(O->getOperator()))) &&
        !ExprMutationAnalyzer::isUnevaluated(E, C) && resultUsed(E, C))
        Message = "do not use the result of an assignment operator";
    if (!Message.empty())
        emitPolicyDiagnostic(*this, N, L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
