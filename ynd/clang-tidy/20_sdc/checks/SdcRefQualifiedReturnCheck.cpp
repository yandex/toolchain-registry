#include "SdcRefQualifiedReturnCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
namespace {
bool subobject(const Expr *E);
bool objectPointer(const Expr *E) {
    if (!E) return false;
    E = E->IgnoreParenImpCasts();
    if (isa<CXXThisExpr>(E)) return true;
    if (const auto *U = dyn_cast<UnaryOperator>(E))
        return U->getOpcode() == UO_AddrOf && subobject(U->getSubExpr());
    return E->getType()->isArrayType() && subobject(E);
}
bool subobject(const Expr *E) {
    if (!E) return false;
    E = E->IgnoreParenImpCasts();
    if (const auto *U = dyn_cast<UnaryOperator>(E))
        return U->getOpcode() == UO_Deref && objectPointer(U->getSubExpr());
    if (const auto *M = dyn_cast<MemberExpr>(E)) {
        const auto *F = dyn_cast<FieldDecl>(M->getMemberDecl());
        return F && !F->getType()->isReferenceType() &&
            (M->isArrow() ? objectPointer(M->getBase()) : subobject(M->getBase()));
    }
    if (const auto *A = dyn_cast<ArraySubscriptExpr>(E)) {
        const Expr *Base = A->getBase()->IgnoreParenImpCasts();
        return Base->getType()->isArrayType() && subobject(Base);
    }
    return false;
}
class ReturnFinder : public RecursiveASTVisitor<ReturnFinder> {
public:
    explicit ReturnFinder(bool Reference) : Reference(Reference) {}
    bool Found = false;
    bool TraverseLambdaExpr(LambdaExpr *) { return true; }
    bool VisitReturnStmt(ReturnStmt *R) {
        Found |= Reference ? subobject(R->getRetValue()) : objectPointer(R->getRetValue());
        return true;
    }
private:
    bool Reference;
};
} // namespace
void SdcRefQualifiedReturnCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcRefQualifiedReturnCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto *F = dyn_cast<FunctionDecl>(D);
    const auto *M = dyn_cast<CXXMethodDecl>(D);
    const auto *R = dyn_cast<CXXRecordDecl>(D);
    SourceLocation L = D->getLocation();
    StringRef Message;
    if (M && M->doesThisDeclarationHaveABody() &&
        (M->getReturnType()->isPointerType() || M->getReturnType()->isReferenceType())) {
        ReturnFinder RF(M->getReturnType()->isReferenceType()); RF.TraverseStmt(M->getBody());
        if (!RF.Found) return;
        bool Good = M->getRefQualifier() == RQ_LValue && !M->isConst();
        if (M->getRefQualifier() == RQ_LValue && M->isConst())
            for (const auto *Other : M->getParent()->methods()) {
                if (Other->getDeclName() != M->getDeclName() || Other->getRefQualifier() != RQ_RValue ||
                    Other->getNumParams() != M->getNumParams() || Other->isVariadic() != M->isVariadic()) continue;
                bool Same = true;
                for (unsigned I = 0; I < M->getNumParams(); ++I)
                    Same &= C.hasSameType(M->getParamDecl(I)->getType(), Other->getParamDecl(I)->getType());
                Good |= Same;
            }
        if (!Good) Message = "ref-qualify a member returning its object or subobject to prevent binding temporaries";
    }
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
