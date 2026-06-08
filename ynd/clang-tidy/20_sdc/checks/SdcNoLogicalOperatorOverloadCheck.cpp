#include "SdcNoLogicalOperatorOverloadCheck.h"

#include "clang/AST/Decl.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcNoLogicalOperatorOverloadCheck::SdcNoLogicalOperatorOverloadCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcNoLogicalOperatorOverloadCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        functionDecl(
            anyOf(
                hasOverloadedOperatorName("&&"),
                hasOverloadedOperatorName("||")
            ),
            unless(isExpansionInSystemHeader())
        ).bind("op"),
        this);
}

void SdcNoLogicalOperatorOverloadCheck::check(const MatchFinder::MatchResult& Result) {
    const auto* FD = Result.Nodes.getNodeAs<FunctionDecl>("op");
    if (!FD)
        return;

    // Skip the template pattern itself — only instantiations (actual usage)
    // are flagged. An uninstantiated template declaration is not a violation.
    if (FD->getDescribedFunctionTemplate())
        return;

    // Clang synthesises internal prototype declarations for some template
    // specialisations; these have no valid source location. Skip them.
    if (!FD->getLocation().isValid())
        return;

    // For non-template functions report only the first (canonical) declaration
    // to avoid a duplicate diagnostic when an out-of-line definition is present.
    if (FD != FD->getCanonicalDecl())
        return;

    StringRef OpName =
        FD->getOverloadedOperator() == OO_AmpAmp ? "operator&&" : "operator||";

    diag(FD->getLocation(),
         "overloading '%0' is prohibited; it breaks short-circuit evaluation semantics")
        << OpName;
}

} // namespace sdc
} // namespace tidy
} // namespace clang
