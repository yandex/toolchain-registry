#include "SdcNoFunctionTemplateSpecializationCheck.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcNoFunctionTemplateSpecializationCheck::SdcNoFunctionTemplateSpecializationCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcNoFunctionTemplateSpecializationCheck::registerMatchers(MatchFinder* Finder) {
    // Match all function declarations; TSK and primary-template filtering is
    // done in check() because isExplicitTemplateSpecialization() has
    // unreliable behaviour with function template specialisations in clang-20.
    Finder->addMatcher(
        functionDecl(unless(isExpansionInSystemHeader())).bind("fn"),
        this);
}

void SdcNoFunctionTemplateSpecializationCheck::check(
    const MatchFinder::MatchResult& Result) {

    const auto* FD = Result.Nodes.getNodeAs<FunctionDecl>("fn");
    if (!FD)
        return;

    // FunctionTemplateSpecializationInfo (which carries the TSK) is stored on
    // the canonical declaration, not on each redeclaration. The canonical decl
    // is an internal Clang prototype with an invalid source location; the
    // user-written declaration follows it in the redecl chain.
    const FunctionDecl* Canonical = FD->getCanonicalDecl();

    // Only flag explicit function template specialisations.
    if (Canonical->getTemplateSpecializationKind() != TSK_ExplicitSpecialization)
        return;

    // Only flag specialisations of function templates.
    // Explicit specialisations of non-template members of class templates
    // (e.g. template<> void MyClass<int>::nonTemplateMethod()) have a null
    // primary template and are not covered by this rule.
    if (!Canonical->getPrimaryTemplate())
        return;

    // Skip the internal Clang prototype itself (invalid source location).
    // We want to report on the user-written declaration.
    if (!FD->getLocation().isValid())
        return;

    // Report only the first user-written declaration. The first user-written
    // decl either has no previous decl (in-class specialisation) or has
    // prev == canonical (the internal Clang prototype for out-of-class specs).
    // A follow-up redecl (e.g. an out-of-line definition after a forward decl)
    // has prev != null && prev != canonical, so we skip it to avoid duplicates.
    const FunctionDecl* Prev = FD->getPreviousDecl();
    if (Prev != nullptr && Prev != Canonical)
        return;

    diag(FD->getLocation(),
         "explicit specialization of function template '%0' is prohibited; "
         "use a function overload instead")
        << FD->getQualifiedNameAsString();
}

} // namespace sdc
} // namespace tidy
} // namespace clang
