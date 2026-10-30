#include "lambda_size_check.h"

#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

void LambdaSizeCheck::storeOptions(ClangTidyOptions::OptionMap& options) {
    Options.store(options, "LineThreshold", lineThreshold_);
}

void LambdaSizeCheck::registerMatchers(MatchFinder* finder) {
    finder->addMatcher(lambdaExpr().bind("lambda"), this);
}

void LambdaSizeCheck::check(const MatchFinder::MatchResult& result) {
    const auto* lambda = result.Nodes.getNodeAs<LambdaExpr>("lambda");
    const CompoundStmt* body = lambda->getCompoundStmtBody();
    const SourceManager& sourceManager = *result.SourceManager;
    // Lines strictly between the braces of the body.
    const unsigned lbraceLine = sourceManager.getSpellingLineNumber(body->getLBracLoc());
    const unsigned rbraceLine = sourceManager.getSpellingLineNumber(body->getRBracLoc());
    const unsigned lines = rbraceLine > lbraceLine ? rbraceLine - lbraceLine - 1 : 0;
    if (lines <= lineThreshold_) {
        return;
    }
    diag(lambda->getBeginLoc(), "lambda body is %0 lines long (threshold %1); turn it into a named function")
        << lines << lineThreshold_;
}

}  // namespace clang::tidy::eats
