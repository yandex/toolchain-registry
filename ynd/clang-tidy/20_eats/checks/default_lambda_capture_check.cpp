#include "default_lambda_capture_check.h"

#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

namespace {

AST_MATCHER(LambdaExpr, hasCaptureDefault) {
    return Node.getCaptureDefault() != LCD_None;
}

}  // namespace

void DefaultLambdaCaptureCheck::registerMatchers(MatchFinder* finder) {
    finder->addMatcher(lambdaExpr(hasCaptureDefault()).bind("lambda"), this);
}

void DefaultLambdaCaptureCheck::check(const MatchFinder::MatchResult& result) {
    const auto* lambda = result.Nodes.getNodeAs<LambdaExpr>("lambda");
    const StringRef capture = lambda->getCaptureDefault() == LCD_ByCopy ? "[=]" : "[&]";
    diag(lambda->getCaptureDefaultLoc(), "default capture '%0'; list the captured entities explicitly") << capture;
}

}  // namespace clang::tidy::eats
