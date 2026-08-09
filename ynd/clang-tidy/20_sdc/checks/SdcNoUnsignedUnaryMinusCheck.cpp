#include "SdcNoUnsignedUnaryMinusCheck.h"

#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcNoUnsignedUnaryMinusCheck::SdcNoUnsignedUnaryMinusCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcNoUnsignedUnaryMinusCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        unaryOperator(hasOperatorName("-"),
                      unless(isExpansionInSystemHeader()))
            .bind("operator"),
        this);
}

void SdcNoUnsignedUnaryMinusCheck::check(
    const MatchFinder::MatchResult& Result) {
    const auto* Operator = Result.Nodes.getNodeAs<UnaryOperator>("operator");
    if (!Operator || Operator->isTypeDependent() ||
        Operator->isValueDependent()) {
        return;
    }

    const Expr* Operand = Operator->getSubExpr()->IgnoreParenImpCasts();
    if (!Operand->getType()->isUnsignedIntegerType()) {
        return;
    }

    const SourceManager& SM = *Result.SourceManager;
    SourceLocation Location = SM.getSpellingLoc(Operator->getOperatorLoc());
    if (Location.isInvalid() || SM.isInSystemHeader(Location) ||
        !ReportedLocations.insert(Location.getRawEncoding()).second) {
        return;
    }

    diag(Location,
         "built-in unary '-' operator should not be applied to an unsigned "
         "expression");
}

} // namespace sdc
} // namespace tidy
} // namespace clang
