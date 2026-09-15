#include "SdcNoUnnamedHeaderNamespaceCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "SdcDeclarationUtils.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
using namespace declaration_detail;
void SdcNoUnnamedHeaderNamespaceCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcNoUnnamedHeaderNamespaceCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    auto &SM = *Result.SourceManager;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    SourceLocation L = D->getLocation();
    StringRef Message;
    // The declaration belongs to the expansion file, even when the namespace
    // token is supplied by a macro body written in a different file. Keep the
    // original location below for the separate targeted/system source policy.
    if (const auto *NS = dyn_cast<NamespaceDecl>(D))
        if (NS->isAnonymousNamespace() && inHeader(SM.getExpansionLoc(L), SM))
            Message = "do not declare unnamed namespaces in header files";
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
