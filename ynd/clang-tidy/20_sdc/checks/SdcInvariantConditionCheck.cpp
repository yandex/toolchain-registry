#include "SdcInvariantConditionCheck.h"
#include "SdcLocalEvidenceUtils.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
namespace {
bool inConstexprIfCondition(const Expr *E, ASTContext &C) {
    auto Node = DynTypedNode::create(*E);
    for (unsigned Depth = 0; Depth < 128; ++Depth) {
        auto Parents = C.getParents(Node);
        if (Parents.size() != 1) return false;
        const auto &Parent = Parents[0];
        if (const auto *I = Parent.get<IfStmt>())
            if (I->isConstexpr() && Node.get<Stmt>() == I->getCond()) return true;
        Node = Parent;
    }
    return false;
}

bool typeRange(const Expr *E, ASTContext &C, llvm::APSInt &Min, llvm::APSInt &Max) {
    const auto *V = local_evidence::directObject(E);
    if (!V || V->getType().isVolatileQualified() || !V->getType()->isIntegerType() ||
        V->getType()->isBooleanType() || !E->getType()->isIntegerType()) return false;
    unsigned From = C.getIntWidth(V->getType()), To = C.getIntWidth(E->getType());
    bool Unsigned = V->getType()->isUnsignedIntegerType();
    if (From > To || (!Unsigned && E->getType()->isUnsignedIntegerType()) ||
        (Unsigned && E->getType()->isSignedIntegerType() && From == To)) return false;
    // An extra sign bit permits comparisons of both unsigned and signed ranges.
    Min = llvm::APSInt(Unsigned ? llvm::APInt(To + 1, 0) :
                      llvm::APInt::getSignedMinValue(From).sext(To + 1), false);
    Max = llvm::APSInt((Unsigned ? llvm::APInt::getMaxValue(From) :
                      llvm::APInt::getSignedMaxValue(From)).zext(To + 1), false);
    return true;
}
bool invariant(const Expr *E, ASTContext &C, bool &Value) {
    if (local_evidence::constantBool(E, C, Value)) return true;
    const auto *B = dyn_cast<BinaryOperator>(E->IgnoreParenImpCasts());
    if (!B || !B->isComparisonOp()) return false;
    const Expr *Variable = B->getLHS(), *Constant = B->getRHS();
    auto Op = B->getOpcode();
    auto Bound = Constant->getIntegerConstantExpr(C);
    if (!Bound) {
        std::swap(Variable, Constant); Bound = Constant->getIntegerConstantExpr(C);
        Op = BinaryOperator::reverseComparisonOp(Op);
    }
    llvm::APSInt Min, Max;
    if (!Bound || !typeRange(Variable, C, Min, Max)) return false;
    llvm::APSInt Limit = Bound->extOrTrunc(Min.getBitWidth()); Limit.setIsUnsigned(false);
    switch (Op) {
    case BO_LT: if (Max < Limit) Value = true; else if (Min >= Limit) Value = false; else return false; break;
    case BO_LE: if (Max <= Limit) Value = true; else if (Min > Limit) Value = false; else return false; break;
    case BO_GT: if (Min > Limit) Value = true; else if (Max <= Limit) Value = false; else return false; break;
    case BO_GE: if (Min >= Limit) Value = true; else if (Max < Limit) Value = false; else return false; break;
    case BO_EQ: case BO_NE:
        if (Limit >= Min && Limit <= Max) return false;
        Value = Op == BO_NE; break;
    default: return false;
    }
    return true;
}
} // namespace
void SdcInvariantConditionCheck::registerMatchers(MatchFinder *Finder) {
    // Project adaptation: compile-time assertions intentionally require constant
    // expressions. Leave failed assertions to the compiler's own diagnostic.
    Finder->addMatcher(stmt(anyOf(ifStmt(), whileStmt(), forStmt(), doStmt(), switchStmt(),
                                 conditionalOperator(), binaryOperator(anyOf(hasOperatorName("&&"), hasOperatorName("||")))),
                            unless(hasAncestor(staticAssertDecl())))
                       .bind("control"), this);
}
void SdcInvariantConditionCheck::check(const MatchFinder::MatchResult &Result) {
    const auto *S = Result.Nodes.getNodeAs<Stmt>("control");
    auto &C = *Result.Context;
    const Expr *Condition = nullptr;
    bool LiteralConstexpr = false;
    if (const auto *I = dyn_cast<IfStmt>(S)) {
        if (I->isConsteval()) return;
        Condition = I->getCond();
        if (I->isConstexpr()) {
            // Owner policy: a bare Boolean literal disguises an unconditional
            // or disabled branch; named capabilities and type traits are exempt.
            if (!isa<CXXBoolLiteralExpr>(Condition->IgnoreParenImpCasts())) return;
            LiteralConstexpr = true;
        }
    } else if (const auto *W = dyn_cast<WhileStmt>(S)) Condition = W->getCond();
    else if (const auto *F = dyn_cast<ForStmt>(S)) Condition = F->getCond();
    else if (const auto *D = dyn_cast<DoStmt>(S)) Condition = D->getCond();
    else if (const auto *Switch = dyn_cast<SwitchStmt>(S)) Condition = Switch->getCond();
    else if (const auto *Q = dyn_cast<ConditionalOperator>(S)) Condition = Q->getCond();
    else if (const auto *B = dyn_cast<BinaryOperator>(S)) Condition = B->getLHS();
    if (!Condition || !isInAnalyzedCode(*Condition, Condition->getExprLoc(), C) ||
        !local_evidence::visibleEvaluation(Condition, C)) return;
    // Exempt the entire condition, including nested logical and conditional
    // operators, but retain runtime checks in the selected body and init-statement.
    if (!LiteralConstexpr && inConstexprIfCondition(Condition, C)) return;
    bool Value;
    if (!invariant(Condition, C, Value)) return;
    bool ConstantValue;
    const bool Constant = local_evidence::constantBool(Condition, C, ConstantValue);
    if (isa<WhileStmt>(S) && Constant && Value) return;
    if (isa<DoStmt>(S) && Constant && !Value && S->getBeginLoc().isMacroID()) return;
    for (const Decl *Instance : Instances.claim(*Condition, Condition->getExprLoc(), C)) {
        if (LiteralConstexpr)
            diagnoseAnalysisInstance(*this, Instance, C, Condition->getExprLoc(),
                "if constexpr condition is the literal %0", Value ? "true" : "false");
        else if (isa<SwitchStmt>(S))
            diagnoseAnalysisInstance(*this, Instance, C, Condition->getExprLoc(),
                "switch controlling expression has a constant value");
        else diagnoseAnalysisInstance(*this, Instance, C, Condition->getExprLoc(),
                "controlling expression is always %0", Value ? "true" : "false");
    }
}
} // namespace clang::tidy::sdc
