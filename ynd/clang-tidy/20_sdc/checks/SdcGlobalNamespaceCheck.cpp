#include "SdcGlobalNamespaceCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
void SdcGlobalNamespaceCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcGlobalNamespaceCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto *V = dyn_cast<VarDecl>(D);
    const auto *F = dyn_cast<FunctionDecl>(D);
    SourceLocation L = D->getLocation();
    StringRef Message;
    if (!D->getDeclContext()->isTranslationUnit() || isa<LinkageSpecDecl>(D) ||
        isa<NamespaceAliasDecl>(D) || isa<StaticAssertDecl>(D) || isa<EmptyDecl>(D)) return;
    if (const auto *NS = dyn_cast<NamespaceDecl>(D)) {
        if (!NS->isInline()) return;
    }
    if (F && (F->isMain() || F->isExternC() ||
        F->getOverloadedOperator() == OO_New || F->getOverloadedOperator() == OO_Array_New ||
        F->getOverloadedOperator() == OO_Delete || F->getOverloadedOperator() == OO_Array_Delete)) return;
    if (V && V->isExternC()) return;
    // A lexical extern-C block also exempts types and using declarations.
    if (const auto *LS = dyn_cast<LinkageSpecDecl>(D->getLexicalDeclContext()))
        if (LS->getLanguage() == LinkageSpecLanguageIDs::C) return;
    if (isa<NamedDecl>(D)) Message = "place this declaration in a namespace";
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
