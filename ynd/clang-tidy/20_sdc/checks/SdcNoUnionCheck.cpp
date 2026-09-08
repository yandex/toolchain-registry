#include "SdcNoUnionCheck.h"
#include "SdcCodeSelection.h"

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

    if (!isInAnalyzedCode(*RD, RD->getBeginLoc(), *Result.Context))
        return;

    for (const Decl* Instance :
         AnalysisInstances.claim(*RD, RD->getBeginLoc(), *Result.Context)) {
        (void)Instance;
        diag(RD->getBeginLoc(),
             "use of 'union' is prohibited; use 'std::variant' instead");
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
