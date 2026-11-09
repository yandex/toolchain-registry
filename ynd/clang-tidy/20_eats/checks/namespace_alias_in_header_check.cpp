#include "namespace_alias_in_header_check.h"

#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

    void NamespaceAliasInHeaderCheck::registerMatchers(MatchFinder* finder_ptr) {
        finder_ptr->addMatcher(namespaceAliasDecl().bind("alias"), this);
    }

    void NamespaceAliasInHeaderCheck::check(const MatchFinder::MatchResult& result) {
        const auto* alias_ptr = result.Nodes.getNodeAs<NamespaceAliasDecl>("alias");
        if (utils::isPresumedLocInHeaderFile(alias_ptr->getLocation(), *result.SourceManager, headerFileExtensions_)) {
            diag(alias_ptr->getLocation(), "namespace alias %0 must be declared in a source file, not a header") << alias_ptr;
        }
    }

} // namespace clang::tidy::eats
