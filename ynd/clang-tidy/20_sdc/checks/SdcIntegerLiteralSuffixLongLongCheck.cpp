#include "SdcIntegerLiteralSuffixLongLongCheck.h"

#include "SdcIntegerLiteralSuffix.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcIntegerLiteralSuffixLongLongCheck::SdcIntegerLiteralSuffixLongLongCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcIntegerLiteralSuffixLongLongCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        integerLiteral(
            unless(isExpansionInSystemHeader()),
            unless(hasAncestor(userDefinedLiteral()))
        ).bind("lit"),
        this);
}

void SdcIntegerLiteralSuffixLongLongCheck::check(
    const MatchFinder::MatchResult& Result) {
    const auto* L = Result.Nodes.getNodeAs<IntegerLiteral>("lit");
    if (!L) {
        return;
    }

    ASTContext& Ctx = *Result.Context;
    auto Parsed = parseIntegerLiteralSuffix(
        L, *Result.SourceManager, Ctx.getLangOpts());
    if (!Parsed.valid) {
        return;
    }
    // Per the rule, the violation is "single L or l"; no L at all and LL are
    // both compliant. lCount == 1 is the only case to warn.
    if (Parsed.lCount != 1) {
        return;
    }

    // Use the actual target type, not the width assumed by an example.
    QualType Type = L->getType();
    if (!Type->isSpecificBuiltinType(BuiltinType::LongLong) &&
        !Type->isSpecificBuiltinType(BuiltinType::ULongLong)) return;

    diag(L->getBeginLoc(),
         "integer literal of type long long shall not use a single L or l "
         "in its suffix");
}

} // namespace sdc
} // namespace tidy
} // namespace clang
