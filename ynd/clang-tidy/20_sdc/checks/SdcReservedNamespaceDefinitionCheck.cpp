#include "SdcReservedNamespaceDefinitionCheck.h"

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

// reserves `std`, `posix`, and any `stdN` (N = one or more digits).
// `std0`, `std1`, ... `std123` are all reserved.
bool isReservedNamespaceName(StringRef Name) {
    if (Name == "std" || Name == "posix") {
        return true;
    }
    if (!Name.starts_with("std")) {
        return false;
    }
    StringRef Tail = Name.drop_front(3);
    if (Tail.empty()) {
        return false; // already handled above, defensive
    }
    for (char C : Tail) {
        if (C < '0' || C > '9') {
            return false;
        }
    }
    return true;
}

// Walks up the lexical-context chain, traversing only through
// NamespaceDecls. Returns the first reserved-named namespace reached, or
// nullptr if the walk hits a non-namespace context first.
//
// The "namespace-only" restriction encodes the intent of rule:
// it forbids adding identifiers *at namespace scope* in std/posix/stdN.
// Local variables, function parameters, class members, and template
// parameters of an entity in std live in non-namespace scope; they're
// part of the enclosing entity (which is itself flagged if it shouldn't
// be there), not separate additions to the reserved namespace.
const NamespaceDecl* enclosingReservedNamespace(const Decl* D) {
    for (const DeclContext* DC = D->getDeclContext(); DC;
         DC = DC->getParent()) {
        const auto* NS = llvm::dyn_cast<NamespaceDecl>(DC);
        if (!NS) {
            return nullptr; // hit a non-namespace context; not at ns scope.
        }
        if (NS->isAnonymousNamespace()) {
            continue;
        }
        if (isReservedNamespaceName(NS->getName())) {
            return NS;
        }
    }
    return nullptr;
}

// Specializations of standard templates are the one thing the C++ standard
// allows user code to introduce inside `std`
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

SdcReservedNamespaceDefinitionCheck::SdcReservedNamespaceDefinitionCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcReservedNamespaceDefinitionCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        namedDecl(
            unless(isExpansionInSystemHeader()),
            unless(isImplicit()))
            .bind("decl"),
        this);
}

void SdcReservedNamespaceDefinitionCheck::check(
    const MatchFinder::MatchResult& Result) {
    const auto* ND = Result.Nodes.getNodeAs<NamedDecl>("decl");
    if (!ND || !ND->getIdentifier()) {
        // Skip unnamed decls (anonymous parameters, anonymous structs,
        // constructors/destructors with operator names, ...). They do not
        // introduce a named identifier into the enclosing namespace.
        return;
    }
    // NamespaceDecls just *open* the namespace; opening `namespace std {}`
    // alone introduces nothing. We only flag what's introduced *inside*.
    // Filter the namespace-open itself by skipping NamespaceDecl whose
    // own name is reserved (the body's members get matched separately).
    if (const auto* NS = llvm::dyn_cast<NamespaceDecl>(ND)) {
        if (NS->getIdentifier() && isReservedNamespaceName(NS->getName())) {
            return;
        }
    }
    if (isTemplateSpecialization(ND)) {
        return;
    }
    const NamespaceDecl* Reserved = enclosingReservedNamespace(ND);
    if (!Reserved) {
        return;
    }
    diag(ND->getLocation(),
         "identifier %0 is defined inside reserved namespace %1; "
         "user code shall not add declarations to `std`, `posix`, or `stdN`")
        << ND << Reserved;
}

} // namespace sdc
} // namespace tidy
} // namespace clang
