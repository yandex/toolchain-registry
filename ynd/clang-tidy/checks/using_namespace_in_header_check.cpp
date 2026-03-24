#include "using_namespace_in_header_check.h"

#include <clang/AST/ASTContext.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::arcadia {

void UsingNamespaceInHeaderCheck::registerMatchers(MatchFinder* finder) {
    finder->addMatcher(usingDirectiveDecl().bind("usingNamespace"), this);
}

void UsingNamespaceInHeaderCheck::check(const MatchFinder::MatchResult& result) {
    const auto* U = result.Nodes.getNodeAs<UsingDirectiveDecl>("usingNamespace");
    SourceLocation loc = U->getBeginLoc();

    if (U->isImplicit() || !loc.isValid())
        return;

    // Get the filename where the using directive is located
    const SourceManager& SM = *result.SourceManager;
    StringRef filename = SM.getFilename(loc);

    // Only check header files
    if (!isHeaderFile(filename))
        return;

    // Do not warn if namespace is a std namespace with user-defined literals.
    // The user-defined literals can only be used with a using directive.
    if (isStdLiteralsNamespace(U->getNominatedNamespace()))
        return;

    diag(loc, "do not use namespace using-directives in header files; "
              "use using-declarations instead");
}

bool UsingNamespaceInHeaderCheck::isStdLiteralsNamespace(const NamespaceDecl* NS) {
    if (!NS->getName().endswith("literals"))
        return false;

    const auto* Parent = dyn_cast_or_null<NamespaceDecl>(NS->getParent());
    if (!Parent)
        return false;

    if (Parent->isStdNamespace())
        return true;

    return Parent->getName() == "literals" && Parent->getParent() &&
           Parent->getParent()->isStdNamespace();
}

bool UsingNamespaceInHeaderCheck::isHeaderFile(StringRef filename) {
    if (filename.empty())
        return false;

    // Check common header file extensions
    return filename.ends_with(".h") ||
           filename.ends_with(".hh") ||
           filename.ends_with(".hpp") ||
           filename.ends_with(".hxx") ||
           filename.ends_with(".h++") ||
           filename.ends_with(".H");
}

}  // namespace clang::tidy::arcadia
