#include "SdcBracesAroundStatementsCheck.h"

#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcBracesAroundStatementsCheck::SdcBracesAroundStatementsCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcBracesAroundStatementsCheck::registerMatchers(MatchFinder* Finder) {
    const auto NoSystem = unless(isExpansionInSystemHeader());
    Finder->addMatcher(ifStmt(NoSystem).bind("if"), this);
    Finder->addMatcher(forStmt(NoSystem).bind("for"), this);
    Finder->addMatcher(cxxForRangeStmt(NoSystem).bind("range_for"), this);
    Finder->addMatcher(whileStmt(NoSystem).bind("while"), this);
    Finder->addMatcher(doStmt(NoSystem).bind("do"), this);
    Finder->addMatcher(switchStmt(NoSystem).bind("switch"), this);
}

void SdcBracesAroundStatementsCheck::check(
    const MatchFinder::MatchResult& Result) {
    auto Diagnose = [this](SourceLocation Loc, StringRef Kind) {
        diag(Loc, "body of %0 statement shall be a compound statement") << Kind;
    };

    if (const auto* S = Result.Nodes.getNodeAs<IfStmt>("if")) {
        if (!isa<CompoundStmt>(S->getThen()))
            Diagnose(S->getIfLoc(), "if");
        // Preserve the conventional else-if chain.  The nested IfStmt is
        // checked separately and its own body must still be compound.
        if (const Stmt* Else = S->getElse())
            if (!isa<CompoundStmt>(Else) && !isa<IfStmt>(Else))
                Diagnose(S->getElseLoc(), "else");
        return;
    }
    if (const auto* S = Result.Nodes.getNodeAs<ForStmt>("for")) {
        if (!isa<CompoundStmt>(S->getBody())) Diagnose(S->getForLoc(), "for");
        return;
    }
    if (const auto* S = Result.Nodes.getNodeAs<CXXForRangeStmt>("range_for")) {
        if (!isa<CompoundStmt>(S->getBody())) Diagnose(S->getForLoc(), "for");
        return;
    }
    if (const auto* S = Result.Nodes.getNodeAs<WhileStmt>("while")) {
        if (!isa<CompoundStmt>(S->getBody())) Diagnose(S->getWhileLoc(), "while");
        return;
    }
    if (const auto* S = Result.Nodes.getNodeAs<DoStmt>("do")) {
        if (!isa<CompoundStmt>(S->getBody())) Diagnose(S->getDoLoc(), "do");
        return;
    }
    if (const auto* S = Result.Nodes.getNodeAs<SwitchStmt>("switch"))
        if (!isa<CompoundStmt>(S->getBody())) Diagnose(S->getSwitchLoc(), "switch");
}

} // namespace sdc
} // namespace tidy
} // namespace clang
