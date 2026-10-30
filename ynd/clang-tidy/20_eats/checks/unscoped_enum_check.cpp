#include "unscoped_enum_check.h"

#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

void UnscopedEnumCheck::registerMatchers(MatchFinder* finder) {
    finder->addMatcher(
        enumDecl(
            isDefinition(),
            unless(isScoped()),
            unless(isInstantiated()),
            unless(hasAncestor(linkageSpecDecl())))
            .bind("enum"),
        this);
}

void UnscopedEnumCheck::check(const MatchFinder::MatchResult& result) {
    const auto* enumeration = result.Nodes.getNodeAs<EnumDecl>("enum");
    diag(enumeration->getLocation(), "use 'enum class' instead of an unscoped enum");
}

}  // namespace clang::tidy::eats
