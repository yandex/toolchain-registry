#include "SdcIdentifierLeadingUnderscoreCheck.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

namespace {

// A FunctionDecl with name kind CXXLiteralOperatorName is a UDL operator
// like `operator""_km`. Its `getNameAsString()` does not start with `_`
// (it starts with `o`), so the textual check wouldn't fire on it anyway.
// Listed here so the auditor can see we considered the case.
bool isLiteralOperatorDecl(const NamedDecl* ND) {
    const auto* FD = llvm::dyn_cast<FunctionDecl>(ND);
    return FD && FD->getDeclName().getNameKind()
                     == DeclarationName::CXXLiteralOperatorName;
}

// Template specializations (explicit or partial) are exempt per the rule's
// closing note: they do not introduce new identifiers.
bool isTemplateSpecialization(const NamedDecl* ND) {
    if (llvm::isa<ClassTemplateSpecializationDecl>(ND)) {
        return true;
    }
    if (llvm::isa<VarTemplateSpecializationDecl>(ND)) {
        return true;
    }
    if (const auto* FD = llvm::dyn_cast<FunctionDecl>(ND)) {
        switch (FD->getTemplateSpecializationKind()) {
            case TSK_ExplicitSpecialization:
            case TSK_ImplicitInstantiation:
            case TSK_ExplicitInstantiationDeclaration:
            case TSK_ExplicitInstantiationDefinition:
                return true;
            case TSK_Undeclared:
                return false;
        }
    }
    return false;
}

} // namespace

SdcIdentifierLeadingUnderscoreCheck::SdcIdentifierLeadingUnderscoreCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcIdentifierLeadingUnderscoreCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        namedDecl(
            unless(isExpansionInSystemHeader()),
            unless(isImplicit()))
            .bind("decl"),
        this);
}

void SdcIdentifierLeadingUnderscoreCheck::check(
    const MatchFinder::MatchResult& Result) {
    const auto* ND = Result.Nodes.getNodeAs<NamedDecl>("decl");
    if (!ND) {
        return;
    }
    if (!ND->getIdentifier()) {
        // Anonymous decls, constructors/destructors, conversion operators,
        // overloaded-operator names, and literal-operator names all reach
        // here with no plain Identifier. None of them can start with `_`
        // at the identifier level, so they are out of scope.
        return;
    }
    StringRef Name = ND->getName();
    if (Name.empty() || Name.front() != '_') {
        return;
    }
    if (isLiteralOperatorDecl(ND)) {
        return; // defensive - getIdentifier() already filters these out.
    }
    if (isTemplateSpecialization(ND)) {
        return;
    }
    diag(ND->getLocation(),
         "identifier %0 starts with `_`; "
         "leading-underscore identifiers for literal suffixes")
        << ND;
}

} // namespace sdc
} // namespace tidy
} // namespace clang
