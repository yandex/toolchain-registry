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
    if (const auto *NS = dyn_cast<NamespaceDecl>(D))
        if (NS->isAnonymousNamespace() && inHeader(L, SM))
            Message = "do not declare unnamed namespaces in header files";
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
