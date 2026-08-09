#include "SdcNoGotoCheck.h"

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

    const SourceManager& SM = *Result.SourceManager;
    SourceLocation Location = SM.getSpellingLoc(Goto->getGotoLoc());
    if (Location.isInvalid() || SM.isInSystemHeader(Location) ||
        !ReportedLocations.insert(Location.getRawEncoding()).second) {
        return;
    }

    diag(Location, "goto statement should not be used");
}

} // namespace sdc
} // namespace tidy
} // namespace clang
