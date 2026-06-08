#include "SdcNoUnionCheck.h"

#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcNoUnionCheck::SdcNoUnionCheck(StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcNoUnionCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        recordDecl(
            isUnion(),
            isDefinition(),
            unless(isExpansionInSystemHeader())
        ).bind("union_decl"),
        this);
}

void SdcNoUnionCheck::check(const MatchFinder::MatchResult& Result) {
    const auto* RD = Result.Nodes.getNodeAs<RecordDecl>("union_decl");
    if (!RD)
        return;

    // Implicit instantiations and explicit instantiation definitions/declarations
    // are compiler-generated from a template definition that is already reported.
    // Explicit specialisations (TSK_ExplicitSpecialization) are user-written and
    // must still be reported.
    if (const auto* CRD = dyn_cast<CXXRecordDecl>(RD)) {
        auto Kind = CRD->getTemplateSpecializationKind();
        if (Kind == TSK_ImplicitInstantiation ||
            Kind == TSK_ExplicitInstantiationDefinition ||
            Kind == TSK_ExplicitInstantiationDeclaration)
            return;
    }

    diag(RD->getBeginLoc(),
         "use of 'union' is prohibited; use 'std::variant' instead");
}

} // namespace sdc
} // namespace tidy
} // namespace clang
