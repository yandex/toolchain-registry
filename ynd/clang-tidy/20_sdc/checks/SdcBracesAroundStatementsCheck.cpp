#include "SdcBracesAroundStatementsCheck.h"
#include "SdcCodeSelection.h"

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
    auto Diagnose = [this, &Result](const Stmt& Node, SourceLocation Loc,
                                    StringRef Kind) {
        for (const Decl* Instance :
             AnalysisInstances.claim(Node, Loc, *Result.Context)) {
            (void)Instance;
            diag(Loc, "body of %0 statement shall be a compound statement")
                << Kind;
        }
    };

    if (const auto* S = Result.Nodes.getNodeAs<IfStmt>("if")) {
        if (!isInAnalyzedCode(*S, S->getIfLoc(), *Result.Context)) return;
        if (!isa<CompoundStmt>(S->getThen()))
            Diagnose(*S, S->getIfLoc(), "if");
        // Preserve the conventional else-if chain.  The nested IfStmt is
        // checked separately and its own body must still be compound.
        if (const Stmt* Else = S->getElse())
            if (!isa<CompoundStmt>(Else) && !isa<IfStmt>(Else))
                Diagnose(*S, S->getElseLoc(), "else");
        return;
    }
    if (const auto* S = Result.Nodes.getNodeAs<ForStmt>("for")) {
        if (!isInAnalyzedCode(*S, S->getForLoc(), *Result.Context)) return;
        if (!isa<CompoundStmt>(S->getBody()))
            Diagnose(*S, S->getForLoc(), "for");
        return;
    }
    if (const auto* S = Result.Nodes.getNodeAs<CXXForRangeStmt>("range_for")) {
        if (!isInAnalyzedCode(*S, S->getForLoc(), *Result.Context)) return;
        if (!isa<CompoundStmt>(S->getBody()))
            Diagnose(*S, S->getForLoc(), "for");
        return;
    }
    if (const auto* S = Result.Nodes.getNodeAs<WhileStmt>("while")) {
        if (!isInAnalyzedCode(*S, S->getWhileLoc(), *Result.Context)) return;
        if (!isa<CompoundStmt>(S->getBody()))
            Diagnose(*S, S->getWhileLoc(), "while");
        return;
    }
    if (const auto* S = Result.Nodes.getNodeAs<DoStmt>("do")) {
        if (!isInAnalyzedCode(*S, S->getDoLoc(), *Result.Context)) return;
        if (!isa<CompoundStmt>(S->getBody()))
            Diagnose(*S, S->getDoLoc(), "do");
        return;
    }
    if (const auto* S = Result.Nodes.getNodeAs<SwitchStmt>("switch")) {
        if (!isInAnalyzedCode(*S, S->getSwitchLoc(), *Result.Context)) return;
        if (!isa<CompoundStmt>(S->getBody()))
            Diagnose(*S, S->getSwitchLoc(), "switch");
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
