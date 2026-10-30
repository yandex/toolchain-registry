#include "namespace_scope_static_check.h"

#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

void NamespaceScopeStaticCheck::registerMatchers(MatchFinder* finder) {
    finder->addMatcher(
        namedDecl(
            anyOf(functionDecl(isStaticStorageClass()), varDecl(isStaticStorageClass())),
            hasDeclContext(anyOf(namespaceDecl(), translationUnitDecl())))
            .bind("decl"),
        this);
}

void NamespaceScopeStaticCheck::check(const MatchFinder::MatchResult& result) {
    const auto* decl = result.Nodes.getNodeAs<NamedDecl>("decl");
    // In a header an anonymous namespace is not allowed either, so the rule applies to source files only.
    if (decl->isInAnonymousNamespace() ||
        utils::isPresumedLocInHeaderFile(decl->getLocation(), *result.SourceManager, headerFileExtensions_)) {
        return;
    }
    diag(decl->getLocation(), "%0 is declared 'static'; move it into an anonymous namespace instead") << decl;
}

}  // namespace clang::tidy::eats
