#include "parameter_count_check.h"

#include <clang/AST/DeclCXX.h>
#include <clang/AST/ExprCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

void ParameterCountCheck::storeOptions(ClangTidyOptions::OptionMap& options) {
    Options.store(options, "ParameterThreshold", parameterThreshold_);
}

void ParameterCountCheck::registerMatchers(MatchFinder* finder) {
    finder->addMatcher(functionDecl(unless(isImplicit())).bind("function"), this);
    // The call operator of a lambda is not spelled in source, so the lambda is matched itself.
    finder->addMatcher(lambdaExpr().bind("lambda"), this);
}

void ParameterCountCheck::check(const MatchFinder::MatchResult& result) {
    if (const auto* lambda = result.Nodes.getNodeAs<LambdaExpr>("lambda")) {
        const unsigned parameters = lambda->getCallOperator()->getNumParams();
        if (parameters > parameterThreshold_) {
            diag(lambda->getBeginLoc(), "lambda has %0 parameters (threshold %1); group the parameters into a struct")
                << parameters << parameterThreshold_;
        }
        return;
    }

    const auto* function = result.Nodes.getNodeAs<FunctionDecl>("function");
    // Redeclarations repeat the first declaration, and an override takes its signature from the base.
    if (function->getCanonicalDecl() != function || function->getNumParams() <= parameterThreshold_) {
        return;
    }
    if (const auto* method = dyn_cast<CXXMethodDecl>(function); method && method->size_overridden_methods() != 0) {
        return;
    }
    diag(function->getLocation(), "function %0 has %1 parameters (threshold %2); split it or group the parameters "
                                  "into a struct")
        << function << function->getNumParams() << parameterThreshold_;
}

}  // namespace clang::tidy::eats
