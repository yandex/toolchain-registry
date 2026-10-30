#include "using_namespace_outside_function_check.h"

#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

void UsingNamespaceOutsideFunctionCheck::registerMatchers(MatchFinder* finder) {
    // A using-directive inside a block or a lambda belongs to the enclosing function.
    finder->addMatcher(usingDirectiveDecl(unless(hasDeclContext(functionDecl()))).bind("directive"), this);
}

void UsingNamespaceOutsideFunctionCheck::check(const MatchFinder::MatchResult& result) {
    const auto* directive = result.Nodes.getNodeAs<UsingDirectiveDecl>("directive");
    diag(directive->getUsingLoc(), "'using namespace' outside a function; move it into the function "
                                   "or use using-declarations");
}

}  // namespace clang::tidy::eats
