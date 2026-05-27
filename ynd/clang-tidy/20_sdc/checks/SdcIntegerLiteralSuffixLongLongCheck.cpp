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

    // examples explicitly assume long is 32-bit. Implement
    // the rule platform-independently by checking whether the literal's
    // value would force the [lex.icon] ladder to (un)signed long long on a
    // 32-bit-long platform. Otherwise on 64-bit-long Linux the rule would
    // never fire on values that ARE long long.
    llvm::APInt Value = L->getValue();
    if (Value.getActiveBits() > 64) {
        return; // defensively skip implausibly-wide literals
    }
    uint64_t V = Value.getZExtValue();

    uint64_t Threshold;
    if (Parsed.base == ParsedIntegerLiteralSuffix::Base::Decimal && !Parsed.hasU) {
        // Decimal-with-L ladder: long -> long long. On 32-bit long the type
        // is long long when value exceeds signed-32-bit max.
        Threshold = 0x7FFFFFFFULL;
    } else {
        // Every other base/suffix combo's ladder has unsigned long in it on
        // the way to long long, so long long kicks in past unsigned-32-bit max.
        Threshold = 0xFFFFFFFFULL;
    }

    if (V <= Threshold) {
        return;
    }

    diag(L->getBeginLoc(),
         "integer literal of type long long shall not use a single L or l "
         "in its suffix");
}

} // namespace sdc
} // namespace tidy
} // namespace clang
