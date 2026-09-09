#include "SdcExpressionUtils.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Analysis/Analyses/ExprMutationAnalyzer.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/SmallPtrSet.h"

namespace clang::tidy::sdc::expression_detail {
const Expr *peel(const Expr *E) {
    if (!E) return nullptr;
    while (true) {
        E = E->IgnoreParenImpCasts();
        if (const auto *M = dyn_cast<MaterializeTemporaryExpr>(E)) E = M->getSubExpr();
        else if (const auto *B = dyn_cast<CXXBindTemporaryExpr>(E)) E = B->getSubExpr();
        else if (const auto *C = dyn_cast<ExprWithCleanups>(E)) E = C->getSubExpr();
        else return E;
    }
}
bool wrapper(const DynTypedNode &N) {
    return N.get<ParenExpr>() || N.get<ImplicitCastExpr>() || N.get<ConstantExpr>() ||
           N.get<ExprWithCleanups>() || N.get<MaterializeTemporaryExpr>() || N.get<CXXBindTemporaryExpr>();
}
DynTypedNode semanticParent(const Expr *E, ASTContext &C) {
    auto N = DynTypedNode::create(*E);
    for (unsigned I = 0; I < 128; ++I) {
        auto P = C.getParents(N);
        if (P.empty()) return {};
        if (!wrapper(P[0])) return P[0];
        N = P[0];
    }
    return {};
}
bool constant(const Expr *E, ASTContext &C, llvm::APSInt &Value) {
    if (!E || E->isValueDependent()) return false;
    auto V = E->getIntegerConstantExpr(C);
    if (!V) return false;
    Value = *V; return true;
}
}
