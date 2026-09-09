#include "SdcUnusedVariablesCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "SdcDeclarationUtils.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
using namespace declaration_detail;
namespace {
class VariableUses : public RecursiveASTVisitor<VariableUses> {
public:
    VariableUses(ASTContext &C, llvm::SmallPtrSetImpl<const VarDecl *> &Used) : C(C), Used(Used) {}
    bool shouldVisitTemplateInstantiations() const { return true; }
    bool VisitDeclRefExpr(DeclRefExpr *E) {
        const auto *V = dyn_cast<VarDecl>(E->getDecl());
        if (const auto *B = dyn_cast<BindingDecl>(E->getDecl())) V = dyn_cast_or_null<VarDecl>(B->getDecomposedDecl());
        if (V && isInMaterializedCode(*E, C)) Used.insert(V->getCanonicalDecl());
        return true;
    }
private:
    ASTContext &C;
    llvm::SmallPtrSetImpl<const VarDecl *> &Used;
};
} // namespace
void SdcUnusedVariablesCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcUnusedVariablesCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    auto &SM = *Result.SourceManager;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto *V = dyn_cast<VarDecl>(D);
    SourceLocation L = D->getLocation();
    StringRef Message;
    if (!CollectedVariableUses) {
        VariableUses Uses(C, UsedVariables); Uses.TraverseDecl(C.getTranslationUnitDecl());
        CollectedVariableUses = true;
    }
    if (V && !isa<ParmVarDecl>(V) && !V->isImplicit() && !V->hasExternalFormalLinkage() &&
        V->getCanonicalDecl() == V && !UsedVariables.contains(V->getCanonicalDecl()) && !V->hasAttr<UnusedAttr>()) {
        if (V->getType().isConstQualified() && namespaceScope(V->getDeclContext()) && inHeader(L, SM)) return;
        if (const auto *VR = V->getType()->getAsCXXRecordDecl()) {
            if (VR->hasDefinition()) {
                bool User = VR->getDestructor() && VR->getDestructor()->isUserProvided();
                for (const auto *VC : VR->ctors()) User |= VC->isUserProvided();
                if (User) return;
            }
        }
        Message = "use this variable at least once or declare it maybe_unused";
    }
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
