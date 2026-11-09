#include "catch_const_reference_check.h"

#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

    void CatchConstReferenceCheck::registerMatchers(MatchFinder* finder_ptr) {
        finder_ptr->addMatcher(cxxCatchStmt().bind("handler"), this);
    }

    void CatchConstReferenceCheck::check(const MatchFinder::MatchResult& result) {
        const auto* handler_ptr = result.Nodes.getNodeAs<CXXCatchStmt>("handler");
        const auto* exception_ptr = handler_ptr->getExceptionDecl();
        if (!exception_ptr) {
            return;
        }
        const QualType type = exception_ptr->getType();
        if (type->isLValueReferenceType() && !type->getPointeeType().isConstQualified()) {
            diag(exception_ptr->getBeginLoc(), "catch exceptions by const reference instead of mutable reference");
        }
    }

} // namespace clang::tidy::eats
