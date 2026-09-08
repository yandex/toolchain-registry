#include "SdcExplicitSingleArgCtorCheck.h"
#include "SdcCodeSelection.h"

#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcExplicitSingleArgCtorCheck::SdcExplicitSingleArgCtorCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcExplicitSingleArgCtorCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        cxxConstructorDecl(
            unless(isExplicit()),
            unless(isCopyConstructor()),
            unless(isMoveConstructor()),
            unless(isImplicit()),
            unless(isDeleted()),
            unless(isExpansionInSystemHeader())
        ).bind("ctor"),
        this
    );

    Finder->addMatcher(
        cxxConversionDecl(
            unless(isExplicit()),
            unless(isImplicit()),
            unless(isDeleted()),
            unless(isExpansionInSystemHeader())
        ).bind("conv"),
        this
    );
}

void SdcExplicitSingleArgCtorCheck::check(const MatchFinder::MatchResult& Result) {
    if (const auto* CD = Result.Nodes.getNodeAs<CXXConstructorDecl>("ctor")) {
        // "callable with a single argument": has ≥1 param and ≤1 required param
        if (CD->getNumParams() == 0) return;
        if (CD->getMinRequiredArguments() > 1) return;
        if (!isInAnalyzedCode(*CD, CD->getLocation(), *Result.Context)) return;

        for (const Decl* Instance : AnalysisInstances.claim(
                 *CD, CD->getLocation(), *Result.Context)) {
            (void)Instance;
            diag(CD->getLocation(),
                 "constructor callable with a single argument shall be declared explicit");
        }
        return;
    }

    if (const auto* Conv = Result.Nodes.getNodeAs<CXXConversionDecl>("conv")) {
        if (!isInAnalyzedCode(*Conv, Conv->getLocation(), *Result.Context)) return;

        for (const Decl* Instance : AnalysisInstances.claim(
                 *Conv, Conv->getLocation(), *Result.Context)) {
            (void)Instance;
            diag(Conv->getLocation(),
                 "conversion operator shall be declared explicit");
        }
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
