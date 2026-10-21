#include "empty_catch_all_check.h"

#include <clang/AST/StmtCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

void EmptyCatchAllCheck::registerMatchers(MatchFinder* finder) {
    // A handler of a constructor or destructor function-try-block rethrows on
    // reaching its end ([except.handle]/15), so an empty one swallows nothing.
    const auto structorFunctionTryBlock =
        cxxTryStmt(hasParent(functionDecl(anyOf(cxxConstructorDecl(), cxxDestructorDecl()))));

    finder->addMatcher(
        cxxCatchStmt(
            isCatchAll(),
            has(compoundStmt(statementCountIs(0))),
            unless(hasParent(structorFunctionTryBlock)))
            .bind("catch"),
        this);
}

void EmptyCatchAllCheck::check(const MatchFinder::MatchResult& result) {
    const auto* catchStmt = result.Nodes.getNodeAs<CXXCatchStmt>("catch");
    diag(catchStmt->getCatchLoc(), "empty 'catch (...)' silently swallows all exceptions; "
                                   "handle, log or rethrow the exception");
}

}  // namespace clang::tidy::eats
