#include "memory_order_check.h"

#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

void MemoryOrderCheck::registerMatchers(MatchFinder* finder) {
    const auto memoryOrderType =
        hasUnqualifiedDesugaredType(enumType(hasDeclaration(enumDecl(hasName("::std::memory_order")))));

    // Matches both the enumerators and the std::memory_order_* constants, but not
    // user variables that only hold a memory order.
    finder->addMatcher(
        declRefExpr(
            hasType(memoryOrderType),
            to(namedDecl(
                anyOf(enumConstantDecl(), varDecl(isInStdNamespace())),
                unless(matchesName("seq_cst$")))
                .bind("order")))
            .bind("ref"),
        this);
}

void MemoryOrderCheck::check(const MatchFinder::MatchResult& result) {
    const auto* ref = result.Nodes.getNodeAs<DeclRefExpr>("ref");
    const auto* order = result.Nodes.getNodeAs<NamedDecl>("order");
    diag(ref->getLocation(), "memory order '%0' weakens the default sequential consistency; "
                             "justify it in a NOLINT comment")
        << order->getName();
}

}  // namespace clang::tidy::eats
