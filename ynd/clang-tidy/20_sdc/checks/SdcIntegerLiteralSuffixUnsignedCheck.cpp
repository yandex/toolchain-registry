#include "SdcIntegerLiteralSuffixUnsignedCheck.h"

#include "SdcIntegerLiteralSuffix.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcIntegerLiteralSuffixUnsignedCheck::SdcIntegerLiteralSuffixUnsignedCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcIntegerLiteralSuffixUnsignedCheck::registerMatchers(MatchFinder* Finder) {
    // UserDefinedLiteral is a different AST node, so integerLiteral() already
    // excludes UDLs.
    Finder->addMatcher(
        integerLiteral(unless(isExpansionInSystemHeader())).bind("lit"),
        this);
}

void SdcIntegerLiteralSuffixUnsignedCheck::check(
    const MatchFinder::MatchResult& Result) {
    const auto* L = Result.Nodes.getNodeAs<IntegerLiteral>("lit");
    if (!L) {
        return;
    }

    QualType T = L->getType();
    if (!T->isUnsignedIntegerType()) {
        return;
    }

    auto Parsed = parseIntegerLiteralSuffix(
        L, *Result.SourceManager, Result.Context->getLangOpts());
    if (!Parsed.valid) {
        return;
    }
    if (Parsed.hasU) {
        return;
    }

    diag(L->getBeginLoc(),
         "unsigned integer literal shall be suffixed with 'u' or 'U'");
}

} // namespace sdc
} // namespace tidy
} // namespace clang
