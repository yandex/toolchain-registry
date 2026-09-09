#include "SdcExternalArraySizeCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
void SdcExternalArraySizeCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcExternalArraySizeCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto *V = dyn_cast<VarDecl>(D);
    SourceLocation L = D->getLocation();
    StringRef Message;
    if (V && V->hasExternalFormalLinkage() && !V->isThisDeclarationADefinition() &&
        V->getTypeSourceInfo()) {
        auto A = V->getTypeSourceInfo()->getTypeLoc().getAs<ArrayTypeLoc>();
        if (A && !A.getSizeExpr()) Message = "specify the size in every non-defining external array declaration";
    }
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
