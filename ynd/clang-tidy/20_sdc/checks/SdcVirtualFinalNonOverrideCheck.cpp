#include "SdcVirtualFinalNonOverrideCheck.h"

#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcVirtualFinalNonOverrideCheck::SdcVirtualFinalNonOverrideCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcVirtualFinalNonOverrideCheck::registerMatchers(MatchFinder* Finder) {
    // Match virtual member functions that have the 'final' specifier but do
    // not override any base class method.  This is the case that
    // modernize-use-override does not cover.
    Finder->addMatcher(
        cxxMethodDecl(
            isVirtual(),
            unless(isExpansionInSystemHeader()),
            unless(isImplicit())
        ).bind("method"),
        this);
}

void SdcVirtualFinalNonOverrideCheck::check(
    const MatchFinder::MatchResult& Result) {

    const auto* MD = Result.Nodes.getNodeAs<CXXMethodDecl>("method");
    if (!MD) return;

    // Only flag methods that:
    //   1. Are declared with the 'virtual' keyword explicitly, AND
    //   2. Have the 'final' specifier, AND
    //   3. Do NOT override any base class method.
    if (!MD->isVirtualAsWritten()) return;
    if (!MD->hasAttr<FinalAttr>())  return;
    if (MD->size_overridden_methods() > 0) return; // overrides something → ok

    // = delete is exempt (no specifier combination makes sense to flag there).
    if (MD->isDeleted()) return;

    diag(MD->getLocation(),
         "'virtual' and 'final' shall not both be specified on a function "
         "that does not override a base class method; use just 'virtual'");
}

} // namespace sdc
} // namespace tidy
} // namespace clang
