#include "SdcInitializerListOnlyConstructorCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/StringRef.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcInitializerListOnlyConstructorCheck::SdcInitializerListOnlyConstructorCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcInitializerListOnlyConstructorCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        cxxRecordDecl(isDefinition(), unless(isExpansionInSystemHeader())).bind("cls"),
        this);
}

namespace {

// Returns true if the DeclContext is the std namespace (or an inline namespace
// inside it).
bool isInStdNamespace(const DeclContext* DC) {
    while (DC && DC->isInlineNamespace())
        DC = DC->getParent();
    const auto* NS = dyn_cast_or_null<NamespaceDecl>(DC);
    return NS && NS->isStdNamespace();
}

// Returns true if T (after stripping reference and cv-qualifiers) is
// std::initializer_list<U> for some U.
bool isInitListType(QualType Q, ASTContext&) {
    if (Q->isReferenceType())
        Q = Q.getNonReferenceType();
    Q = Q.getCanonicalType().getUnqualifiedType();

    const auto* RD = Q->getAsCXXRecordDecl();
    if (!RD) return false;
    const auto* Spec = dyn_cast<ClassTemplateSpecializationDecl>(RD);
    if (!Spec) return false;
    if (Spec->getName() != "initializer_list") return false;
    return isInStdNamespace(Spec->getDeclContext());
}

// Returns true when the constructor qualifies as an initializer-list constructor
// per the C++ definition:
//   - First parameter is std::initializer_list<T> or reference to cv-qualified one.
//   - All remaining parameters have default arguments.
bool isInitializerListCtor(const CXXConstructorDecl* Ctor, ASTContext& Ctx) {
    if (Ctor->getNumParams() == 0) return false;
    ASTContext& CtxRef = Ctx; // kept for signature symmetry
    if (!isInitListType(Ctor->getParamDecl(0)->getType(), CtxRef)) return false;
    for (unsigned i = 1; i < Ctor->getNumParams(); ++i) {
        if (!Ctor->getParamDecl(i)->hasDefaultArg()) return false;
    }
    return true;
}

} // namespace

void SdcInitializerListOnlyConstructorCheck::check(
    const MatchFinder::MatchResult& Result) {

    const auto* RD = Result.Nodes.getNodeAs<CXXRecordDecl>("cls");
    if (!RD) return;

    // Skip template patterns: constructors in uninstantiated templates may have
    // dependent parameter types that cannot be resolved yet.
    if (RD->getDescribedClassTemplate()) return;
    // Skip instantiations to avoid duplicate diagnostics.
    if (RD->getTemplateSpecializationKind() != TSK_Undeclared) return;

    ASTContext& Ctx = *Result.Context;

    // Partition user-declared, non-deleted constructors.
    llvm::SmallVector<const CXXConstructorDecl*, 4> InitListCtors;
    llvm::SmallVector<const CXXConstructorDecl*, 4> OtherCtors;

    for (const CXXConstructorDecl* Ctor : RD->ctors()) {
        if (Ctor->isImplicit()) continue;  // compiler-generated
        if (Ctor->isDeleted())  continue;  // deleted — not "defined"

        if (Ctor->isCopyConstructor() || Ctor->isMoveConstructor()) continue;

        if (isInitializerListCtor(Ctor, Ctx))
            InitListCtors.push_back(Ctor->getCanonicalDecl());
        else
            OtherCtors.push_back(Ctor->getCanonicalDecl());
    }

    if (InitListCtors.empty() || OtherCtors.empty()) return;

    // Violation: initializer-list constructor coexists with other constructors.
    for (const CXXConstructorDecl* Other : OtherCtors) {
        diag(Other->getLocation(),
             "constructor '%0' cannot coexist with an initializer-list "
             "constructor in the same class; only copy and move constructors "
             "are permitted alongside an initializer-list constructor")
            << Other->getNameAsString();
        for (const CXXConstructorDecl* IL : InitListCtors)
            diag(IL->getLocation(),
                 "initializer-list constructor defined here",
                 DiagnosticIDs::Note);
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
