#include "SdcNoAddressofOperatorOverloadCheck.h"

#include "clang/AST/Decl.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcNoAddressofOperatorOverloadCheck::SdcNoAddressofOperatorOverloadCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcNoAddressofOperatorOverloadCheck::registerMatchers(MatchFinder* Finder) {
    // operator& is overloaded for both address-of (unary) and bitwise-AND (binary).
    // Only the unary form is banned:
    //   - member:     T* operator&()       — 0 explicit parameters
    //   - free func:  T* operator&(T&)     — 1 parameter
    Finder->addMatcher(
        functionDecl(
            hasOverloadedOperatorName("&"),
            anyOf(
                allOf(cxxMethodDecl(), parameterCountIs(0)),
                allOf(unless(cxxMethodDecl()), parameterCountIs(1))
            ),
            unless(isExpansionInSystemHeader())
        ).bind("op"),
        this);
}

void SdcNoAddressofOperatorOverloadCheck::check(const MatchFinder::MatchResult& Result) {
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

    // Report only the first (canonical) declaration to avoid a duplicate
    // diagnostic when an out-of-line definition is also present.
    if (FD != FD->getCanonicalDecl())
        return;

    diag(FD->getLocation(),
         "overloading 'operator&' (address-of) is prohibited; "
         "use 'std::addressof' to obtain the address of an object");
}

} // namespace sdc
} // namespace tidy
} // namespace clang
