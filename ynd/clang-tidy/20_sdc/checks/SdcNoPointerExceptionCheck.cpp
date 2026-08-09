#include "SdcNoPointerExceptionCheck.h"

#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcNoPointerExceptionCheck::SdcNoPointerExceptionCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcNoPointerExceptionCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        cxxThrowExpr(has(expr().bind("operand")),
                     unless(isExpansionInSystemHeader()))
            .bind("throw"),
        this);
}

void SdcNoPointerExceptionCheck::check(
    const MatchFinder::MatchResult& Result) {
    const auto* Throw = Result.Nodes.getNodeAs<CXXThrowExpr>("throw");
    const auto* Operand = Result.Nodes.getNodeAs<Expr>("operand");
    if (!Throw || !Operand || !Operand->getType()->isPointerType()) {
        return;
    }

    diag(Throw->getThrowLoc(),
         "exception object has pointer type '%0'; throw an object value instead")
        << Operand->getType();
}

} // namespace sdc
} // namespace tidy
} // namespace clang
