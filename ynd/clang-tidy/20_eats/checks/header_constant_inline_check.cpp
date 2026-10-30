#include "header_constant_inline_check.h"

#include <clang/AST/DeclTemplate.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

void HeaderConstantInlineCheck::registerMatchers(MatchFinder* finder) {
    finder->addMatcher(
        varDecl(
            unless(parmVarDecl()),
            hasDeclContext(anyOf(namespaceDecl(), translationUnitDecl())),
            anyOf(isConstexpr(), hasType(isConstQualified())))
            .bind("constant"),
        this);
}

void HeaderConstantInlineCheck::check(const MatchFinder::MatchResult& result) {
    const auto* constant = result.Nodes.getNodeAs<VarDecl>("constant");
    // An extern declaration is defined in a single source file, and a variable template
    // has no one-definition problem. An anonymous namespace in a header is reported by
    // google-build-namespaces, and 'inline' does not fix it.
    if (constant->hasExternalStorage() || constant->getDescribedVarTemplate() ||
        isa<VarTemplateSpecializationDecl>(constant) || constant->isInAnonymousNamespace() ||
        !utils::isPresumedLocInHeaderFile(constant->getLocation(), *result.SourceManager, headerFileExtensions_)) {
        return;
    }
    if (constant->getStorageClass() == SC_Static) {
        diag(constant->getLocation(), "namespace-scope constant %0 in a header is 'static' and gets a copy "
                                      "in every translation unit; drop 'static' and declare it 'inline'")
            << constant;
        return;
    }
    if (!constant->isInline()) {
        diag(constant->getLocation(), "namespace-scope constant %0 in a header must be 'inline'") << constant;
    }
}

}  // namespace clang::tidy::eats
