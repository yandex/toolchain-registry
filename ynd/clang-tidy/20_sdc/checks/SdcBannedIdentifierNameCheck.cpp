#include "SdcBannedIdentifierNameCheck.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/StringSet.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

namespace {

// List of reserved names that should not be used
const llvm::StringSet<>& bannedNames() {
    static const llvm::StringSet<> Set{
        "defined",
        "final",
        "override",
        "clock_t",
        "div_t",
        "FILE",
        "fpos_t",
        "lconv",
        "ldiv_t",
        "mbstate_t",
        "ptrdiff_t",
        "sig_atomic_t",
        "size_t",
        "time_t",
        "tm",
        "va_list",
        "wctrans_t",
        "wctype_t",
        "wint_t",
    };
    return Set;
}

// Per the rule note, template specializations do not introduce new
// identifiers and are exempt.
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

SdcBannedIdentifierNameCheck::SdcBannedIdentifierNameCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcBannedIdentifierNameCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        namedDecl(
            unless(isExpansionInSystemHeader()),
            unless(isImplicit()))
            .bind("decl"),
        this);
}

void SdcBannedIdentifierNameCheck::check(
    const MatchFinder::MatchResult& Result) {
    const auto* ND = Result.Nodes.getNodeAs<NamedDecl>("decl");
    if (!ND || !ND->getIdentifier()) {
        return;
    }
    StringRef Name = ND->getName();
    if (!bannedNames().contains(Name)) {
        return;
    }
    if (isTemplateSpecialization(ND)) {
        return;
    }
    diag(ND->getLocation(),
         "identifier %0 is on reserved-name list and "
         "shall not be introduced by user code")
        << ND;
}

} // namespace sdc
} // namespace tidy
} // namespace clang
