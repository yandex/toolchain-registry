#include "SdcNoGotoCheck.h"
#include "SdcCodeSelection.h"

#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcNoGotoCheck::SdcNoGotoCheck(StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcNoGotoCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        gotoStmt(unless(isExpansionInSystemHeader())).bind("goto"), this);
}

void SdcNoGotoCheck::check(const MatchFinder::MatchResult& Result) {
    const auto* Goto = Result.Nodes.getNodeAs<GotoStmt>("goto");
    if (!Goto) {
        return;
    }
    if (!isInAnalyzedCode(*Goto, Goto->getGotoLoc(), *Result.Context)) {
        return;
    }

    const SourceManager& SM = *Result.SourceManager;
    SourceLocation Location = SM.getSpellingLoc(Goto->getGotoLoc());
    for (const Decl* Instance :
         AnalysisInstances.claim(*Goto, Goto->getGotoLoc(), *Result.Context)) {
        (void)Instance;
        diag(Location, "goto statement should not be used");
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
