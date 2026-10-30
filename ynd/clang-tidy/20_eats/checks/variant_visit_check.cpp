#include "variant_visit_check.h"

#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

void VariantVisitCheck::registerMatchers(MatchFinder* finder) {
    finder->addMatcher(
        callExpr(callee(functionDecl(hasAnyName("::std::holds_alternative", "::std::get_if")).bind("function")))
            .bind("call"),
        this);
}

void VariantVisitCheck::check(const MatchFinder::MatchResult& result) {
    const auto* call = result.Nodes.getNodeAs<CallExpr>("call");
    const auto* function = result.Nodes.getNodeAs<FunctionDecl>("function");
    diag(call->getBeginLoc(), "handle std::variant alternatives with std::visit instead of 'std::%0'")
        << function->getName();
}

}  // namespace clang::tidy::eats
