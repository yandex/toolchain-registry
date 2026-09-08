#include "SdcNoBuiltInUnaryPlusCheck.h"
#include "SdcCodeSelection.h"

#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcNoBuiltInUnaryPlusCheck::SdcNoBuiltInUnaryPlusCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcNoBuiltInUnaryPlusCheck::registerMatchers(MatchFinder* Finder) {
    // Overloaded unary + is represented by CXXOperatorCallExpr, so this
    // matcher selects only the built-in operator prohibited by the rule.
    Finder->addMatcher(
        unaryOperator(hasOperatorName("+"),
                      unless(isExpansionInSystemHeader()))
            .bind("operator"),
        this);
}

void SdcNoBuiltInUnaryPlusCheck::check(
    const MatchFinder::MatchResult& Result) {
    const auto* Operator = Result.Nodes.getNodeAs<UnaryOperator>("operator");
    if (!Operator || Operator->isTypeDependent() ||
        Operator->isValueDependent()) {
        return;
    }

    const SourceManager& SM = *Result.SourceManager;
    SourceLocation Location = SM.getSpellingLoc(Operator->getOperatorLoc());
    if (!isInAnalyzedCode(*Operator, Operator->getOperatorLoc(),
                          *Result.Context)) {
        return;
    }

    for (const Decl* Instance : AnalysisInstances.claim(
             *Operator, Operator->getOperatorLoc(), *Result.Context)) {
        (void)Instance;
        diag(Location, "built-in unary '+' operator should not be used");
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
