#include "SdcExplicitLambdaCaptureCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "SdcExpressionUtils.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
using namespace expression_detail;
namespace {
// A lambda is non-storing precisely when every parameter use is a call or
// another non-storing argument. Revisited recursive edges are checked by the
// enclosing traversal, avoiding an arbitrary recursion-depth approximation.
using ParameterSet = llvm::SmallPtrSet<const ParmVarDecl *, 16>;
bool nonStoring(const FunctionDecl *F, unsigned Index, ASTContext &C, ParameterSet &Active);
bool safeLambdaUse(const Expr *E, ASTContext &C, ParameterSet &Active) {
    DynTypedNode P = semanticParent(E, C);
    while (const auto *Construct = P.get<CXXConstructExpr>()) {
        if (!Construct->getConstructor()->isCopyOrMoveConstructor()) return false;
        P = semanticParent(Construct, C);
    }
    const auto *Call = P.get<CallExpr>();
    if (!Call) return false;
    if (const auto *O = dyn_cast<CXXOperatorCallExpr>(Call))
        if (O->getOperator() == OO_Call && O->getNumArgs() && peel(O->getArg(0)) == peel(E)) return true;
    if (peel(Call->getCallee()) == peel(E)) return true;
    const auto *F = Call->getDirectCallee();
    if (!F) return false;
    if (F->isInStdNamespace() && F->getIdentifier() &&
        (F->getName() == "move" || F->getName() == "forward"))
        return safeLambdaUse(Call, C, Active);
    for (unsigned I = 0; I < Call->getNumArgs(); ++I) {
        const Expr *A = peel(Call->getArg(I));
        while (const auto *Construct = dyn_cast<CXXConstructExpr>(A)) {
            if (!Construct->getConstructor()->isCopyOrMoveConstructor() || !Construct->getNumArgs()) break;
            A = peel(Construct->getArg(0));
        }
        if (A == peel(E)) {
            unsigned Offset = isa<CXXOperatorCallExpr>(Call) && isa<CXXMethodDecl>(F) ? 1 : 0;
            return I >= Offset && nonStoring(F, I - Offset, C, Active);
        }
    }
    return false;
}
class ParameterUses : public RecursiveASTVisitor<ParameterUses> {
public:
    ParameterUses(const ParmVarDecl *P, ASTContext &C, ParameterSet &Active) : P(P), C(C), Active(Active) {}
    bool Safe = true;
    bool VisitDeclRefExpr(DeclRefExpr *E) {
        if (E->getDecl() == P && !safeLambdaUse(E, C, Active)) Safe = false;
        return true;
    }
private:
    const ParmVarDecl *P;
    ASTContext &C;
    ParameterSet &Active;
};
bool nonStoring(const FunctionDecl *F, unsigned Index, ASTContext &C, ParameterSet &Active) {
    const FunctionDecl *Def = nullptr;
    if (!F->hasBody(Def) || Index >= Def->getNumParams()) return false;
    const auto *P = Def->getParamDecl(Index);
    if (!Active.insert(P).second) return true;
    ParameterUses V(P, C, Active); V.TraverseStmt(Def->getBody());
    Active.erase(P); return V.Safe;
}
} // namespace
void SdcExplicitLambdaCaptureCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(expr().bind("expr"), this);
}
void SdcExplicitLambdaCaptureCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *E = Result.Nodes.getNodeAs<Expr>("expr");
    if (!E) return;
    SourceLocation L = E->getExprLoc();
    auto N = DynTypedNode::create(*E);
    if (!isInAnalyzedCode(N, L, C)) return;
    StringRef Message;
    if (const auto *LE = dyn_cast<LambdaExpr>(E)) {
        bool Implicit = false;
        for (const auto &Cap : LE->captures()) Implicit |= Cap.isImplicit() && Cap.capturesVariable();
        ParameterSet Active;
        if (Implicit && !safeLambdaUse(LE, C, Active)) {
            L = LE->getBeginLoc(); Message = "capture variables explicitly in a non-transient lambda";
        }
    }
    if (!Message.empty())
        emitPolicyDiagnostic(*this, N, L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
