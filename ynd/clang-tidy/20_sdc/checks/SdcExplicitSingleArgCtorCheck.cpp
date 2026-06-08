#include "SdcExplicitSingleArgCtorCheck.h"

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
        // Skip implicit instantiations — warn only on the template pattern
        if (CD->getTemplateSpecializationKind() == TSK_ImplicitInstantiation)
            return;

        // "callable with a single argument": has ≥1 param and ≤1 required param
        if (CD->getNumParams() == 0) return;
        if (CD->getMinRequiredArguments() > 1) return;

        diag(CD->getLocation(),
             "constructor callable with a single argument shall be declared explicit");
        return;
    }

    if (const auto* Conv = Result.Nodes.getNodeAs<CXXConversionDecl>("conv")) {
        if (Conv->getTemplateSpecializationKind() == TSK_ImplicitInstantiation)
            return;

        diag(Conv->getLocation(),
             "conversion operator shall be declared explicit");
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
