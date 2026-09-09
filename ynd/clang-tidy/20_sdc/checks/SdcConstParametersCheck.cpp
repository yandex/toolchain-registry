#include "SdcConstParametersCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Analysis/Analyses/ExprMutationAnalyzer.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
namespace {
bool nonConstTarget(QualType T) {
    return (T->isPointerType() || T->isReferenceType()) && !T->getPointeeType().isConstQualified();
}
class ParameterEscape : public RecursiveASTVisitor<ParameterEscape> {
public:
    ParameterEscape(const ParmVarDecl *P, const FunctionDecl *F, ASTContext &C) : P(P), F(F), C(C) {}
    bool Escapes = false;
    bool VisitDeclRefExpr(DeclRefExpr *E) {
        if (E->getDecl() != P) return true;
        auto N = DynTypedNode::create(*E);
        for (unsigned I = 0; I < 64; ++I) {
            auto Parents = C.getParents(N); if (Parents.empty()) break;
            const auto &Parent = Parents[0];
            if (const auto *V = Parent.get<VarDecl>()) {
                Escapes |= nonConstTarget(V->getType()); break;
            }
            if (Parent.get<ReturnStmt>()) { Escapes |= nonConstTarget(F->getReturnType()); break; }
            if (const auto *B = Parent.get<BinaryOperator>()) {
                if (B->isAssignmentOp()) {
                    Escapes |= N.get<Expr>() == B->getRHS() && nonConstTarget(B->getLHS()->getType());
                    break;
                }
                break;
            }
            if (Parent.get<CallExpr>() || Parent.get<CompoundStmt>() || Parent.get<Decl>()) break;
            N = Parent;
        }
        return true;
    }
private:
    const ParmVarDecl *P;
    const FunctionDecl *F;
    ASTContext &C;
};
bool templateScope(const DeclContext *DC) {
    for (; DC; DC = DC->getParent()) {
        if (const auto *R = dyn_cast<CXXRecordDecl>(DC))
            if (R->getDescribedClassTemplate() || isa<ClassTemplateSpecializationDecl>(R)) return true;
        if (const auto *F = dyn_cast<FunctionDecl>(DC))
            if (F->getTemplatedKind() != FunctionDecl::TK_NonTemplate) return true;
    }
    return false;
}
} // namespace
void SdcConstParametersCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcConstParametersCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto *V = dyn_cast<VarDecl>(D);
    const auto *F = dyn_cast<FunctionDecl>(D);
    const auto *R = dyn_cast<CXXRecordDecl>(D);
    SourceLocation L = D->getLocation();
    StringRef Message;
    if (const auto *P = dyn_cast<ParmVarDecl>(D)) {
        const auto *Fn = dyn_cast<FunctionDecl>(P->getDeclContext());
        if (!Fn || !Fn->doesThisDeclarationHaveABody() || Fn->isMain() || !P->getIdentifier() ||
            templateScope(Fn) || (isa<CXXMethodDecl>(Fn) && cast<CXXMethodDecl>(Fn)->isVirtual())) return;
        QualType T = P->getType();
        if (!T->isPointerType() && !T->isLValueReferenceType()) return;
        QualType Target = T->getPointeeType();
        if (Target.isConstQualified() || !Target->isObjectType()) return;
        ExprMutationAnalyzer A(*Fn->getBody(), C);
        bool Modified = T->isPointerType() ? A.isPointeeMutated(P) : A.isMutated(P);
        ParameterEscape Escape(P, Fn, C); Escape.TraverseStmt(Fn->getBody());
        if (!Modified && !Escape.Escapes) Message = "const-qualify the target type of this parameter";
    }
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
