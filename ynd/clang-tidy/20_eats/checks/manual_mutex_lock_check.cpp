#include "manual_mutex_lock_check.h"

#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

void ManualMutexLockCheck::registerMatchers(MatchFinder* finder) {
    finder->addMatcher(
        cxxMemberCallExpr(
            callee(cxxMethodDecl(hasAnyName(
                "lock", "unlock", "try_lock", "try_lock_for", "try_lock_until",
                "lock_shared", "unlock_shared", "try_lock_shared", "try_lock_shared_for",
                "try_lock_shared_until")).bind("method")),
            thisPointerType(cxxRecordDecl(matchesName("[Mm]utex$"))))
            .bind("call"),
        this);
}

void ManualMutexLockCheck::check(const MatchFinder::MatchResult& result) {
    const auto* call = result.Nodes.getNodeAs<CXXMemberCallExpr>("call");
    const auto* method = result.Nodes.getNodeAs<CXXMethodDecl>("method");
    diag(call->getExprLoc(), "manual '%0' of a mutex; use std::lock_guard, std::unique_lock or std::scoped_lock")
        << method->getName();
}

}  // namespace clang::tidy::eats
