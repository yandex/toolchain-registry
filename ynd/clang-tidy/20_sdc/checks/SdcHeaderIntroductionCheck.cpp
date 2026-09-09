#include "SdcHeaderIntroductionCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "SdcDeclarationUtils.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
using namespace declaration_detail;
void SdcHeaderIntroductionCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcHeaderIntroductionCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    auto &SM = *Result.SourceManager;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto *V = dyn_cast<VarDecl>(D);
    const auto *F = dyn_cast<FunctionDecl>(D);
    SourceLocation L = D->getLocation();
    StringRef Message;
    if (((V && V->getCanonicalDecl() == V && V->hasExternalFormalLinkage()) ||
         (F && F->getCanonicalDecl() == F && F->hasExternalFormalLinkage() && !F->isMain())) &&
        namespaceScope(D->getDeclContext()) && !inHeader(L, SM))
        Message = "introduce externally linked functions and objects in a header file";
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
