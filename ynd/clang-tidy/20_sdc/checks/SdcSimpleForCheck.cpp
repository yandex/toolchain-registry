#include "SdcSimpleForCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "SdcExpressionUtils.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Analysis/Analyses/ExprMutationAnalyzer.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
using namespace expression_detail;
namespace {
const VarDecl *variable(const Expr *E) {
    if (!E) return nullptr;
    if (const auto *R = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts())) return dyn_cast<VarDecl>(R->getDecl());
    return nullptr;
}
class ReferenceSearch : public RecursiveASTVisitor<ReferenceSearch> {
public:
    explicit ReferenceSearch(const VarDecl *Variable) : Variable(Variable) {}
    bool Found = false;
    bool VisitDeclRefExpr(DeclRefExpr *E) { Found |= E->getDecl() == Variable; return true; }
private:
    const VarDecl *Variable;
};
class LoopAliases : public RecursiveASTVisitor<LoopAliases> {
public:
    explicit LoopAliases(const VarDecl *Variable) : Variable(Variable) {}
    bool Found = false;
    bool VisitVarDecl(VarDecl *D) {
        if (D->hasInit() && mutableTarget(D->getType())) Found |= references(D->getInit());
        return true;
    }
    bool VisitCallExpr(CallExpr *Call) {
        const auto *F = Call->getDirectCallee();
        if (!F) return true;
        unsigned Offset = isa<CXXOperatorCallExpr>(Call) && isa<CXXMethodDecl>(F) ? 1 : 0;
        for (unsigned I = 0; I < F->getNumParams() && I+Offset < Call->getNumArgs(); ++I)
            if (mutableTarget(F->getParamDecl(I)->getType())) Found |= references(Call->getArg(I+Offset));
        return true;
    }
private:
    static bool mutableTarget(QualType T) {
        return (T->isPointerType() || T->isReferenceType()) && !T->getPointeeType().isConstQualified();
    }
    bool references(const Expr *E) {
        ReferenceSearch Search(Variable); Search.TraverseStmt(const_cast<Expr *>(E)); return Search.Found;
    }
    const VarDecl *Variable;
};
bool loopAlias(const VarDecl *V, const ForStmt *F) {
    if (!V) return false;
    LoopAliases Aliases(V); Aliases.TraverseStmt(const_cast<ForStmt *>(F)); return Aliases.Found;
}
bool simpleFor(const ForStmt *F, ASTContext &C) {
    const auto *DS = dyn_cast_or_null<DeclStmt>(F->getInit());
    if (!DS || !DS->isSingleDecl()) return false;
    const auto *Counter = dyn_cast<VarDecl>(DS->getSingleDecl());
    if (!Counter || !Counter->hasInit() || !Counter->getType()->isIntegerType()) return false;
    const auto *Cond = dyn_cast_or_null<BinaryOperator>(F->getCond() ? F->getCond()->IgnoreParenImpCasts() : nullptr);
    if (!Cond || !Cond->isRelationalOp()) return false;
    const Expr *Bound = nullptr;
    if (variable(Cond->getLHS()) == Counter) Bound = Cond->getRHS()->IgnoreParenImpCasts();
    else if (variable(Cond->getRHS()) == Counter) Bound = Cond->getLHS()->IgnoreParenImpCasts();
    else return false;
    llvm::APSInt B;
    bool ConstBound = constant(Bound, C, B);
    if (!ConstBound && !variable(Bound)) return false;
    if (!C.hasSameUnqualifiedType(Counter->getType(), Bound->getType())) {
        if (!ConstBound) return false;
        unsigned Width = C.getIntWidth(Counter->getType());
        if (Counter->getType()->isUnsignedIntegerType()) {
            if (B.isNegative() || B.getActiveBits() > Width) return false;
        } else if ((B.isUnsigned() ? B.getActiveBits() + 1 : B.getSignificantBits()) > Width) return false;
    }
    const Expr *Inc = F->getInc() ? F->getInc()->IgnoreParenImpCasts() : nullptr;
    const Expr *Step = nullptr;
    if (const auto *U = dyn_cast_or_null<UnaryOperator>(Inc)) {
        if (!U->isIncrementDecrementOp() || variable(U->getSubExpr()) != Counter) return false;
    } else if (const auto *A = dyn_cast_or_null<BinaryOperator>(Inc)) {
        if (variable(A->getLHS()) != Counter) return false;
        if (A->getOpcode() == BO_AddAssign || A->getOpcode() == BO_SubAssign) Step = A->getRHS();
        else if (A->getOpcode() == BO_Assign) {
            const auto *Op = dyn_cast<BinaryOperator>(A->getRHS()->IgnoreParenImpCasts());
            if (!Op || (Op->getOpcode() != BO_Add && Op->getOpcode() != BO_Sub)) return false;
            if (variable(Op->getLHS()) == Counter) Step = Op->getRHS();
            else if (Op->getOpcode() == BO_Add && variable(Op->getRHS()) == Counter) Step = Op->getLHS();
            else return false;
        } else return false;
    } else return false;
    ExprMutationAnalyzer Body(*F->getBody(), C);
    if (Body.isMutated(Counter) || loopAlias(Counter, F)) return false;
    ExprMutationAnalyzer Whole(*F, C);
    if (!ConstBound && (Whole.isMutated(variable(Bound)) || loopAlias(variable(Bound), F))) return false;
    llvm::APSInt S;
    if (Step && !constant(Step, C, S)) {
        const auto *SV = variable(Step);
        if (!SV || Whole.isMutated(SV) || loopAlias(SV, F)) return false;
    }
    return true;
}
} // namespace
void SdcSimpleForCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(forStmt().bind("for"), this);
}
void SdcSimpleForCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *F = Result.Nodes.getNodeAs<ForStmt>("for");
    if (!F) return;
    SourceLocation L = F->getForLoc();
    auto N = DynTypedNode::create(*F);
    if (!isInAnalyzedCode(N, L, C)) return;
    StringRef Message;
    if (!simpleFor(F, C)) Message = "use a simple legacy for loop with an integer counter and invariant bound and step";
    if (!Message.empty())
        emitPolicyDiagnostic(*this, N, L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
