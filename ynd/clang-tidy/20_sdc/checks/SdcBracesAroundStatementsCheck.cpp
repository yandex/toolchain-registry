#include "SdcPolicyDiagnostic.h"
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
    : SdcPolicyCheck(Name, Context) {}

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
                                    StringRef Kind, const Stmt* Body) {
        for (const Decl* Instance :
             AnalysisInstances.claim(Node, Loc, *Result.Context)) {

            if (!shouldReportPolicyDiagnostic(Body->getBeginLoc(), *Result.SourceManager)) continue;
            diag(Body->getBeginLoc(), "enclose this %0 body in braces%1")
                << Kind << analysisInstanceSuffix(Instance, *Result.Context)
                << Body->getSourceRange();
            diag(Loc, "%0 statement starts here", DiagnosticIDs::Note) << Kind;
            SourceLocation Point;
            if (const auto* Function = dyn_cast_or_null<FunctionDecl>(Instance))
                Point = Function->getPointOfInstantiation();
            else if (const auto* Class = dyn_cast_or_null<ClassTemplateSpecializationDecl>(Instance))
                Point = Class->getPointOfInstantiation();
            else if (const auto* Variable = dyn_cast_or_null<VarTemplateSpecializationDecl>(Instance))
                Point = Variable->getPointOfInstantiation();
            if (Point.isValid())
                diag(Point, "in instantiation of %0 requested here", DiagnosticIDs::Note)
                    << cast<NamedDecl>(Instance);
        }
    };

    if (const auto* S = Result.Nodes.getNodeAs<IfStmt>("if")) {
        if (!isInAnalyzedCode(*S, S->getIfLoc(), *Result.Context)) return;
        if (!isa<CompoundStmt>(S->getThen()))
            Diagnose(*S, S->getIfLoc(), "if", S->getThen());
        // Preserve the conventional else-if chain.  The nested IfStmt is
        // checked separately and its own body must still be compound.
        if (const Stmt* Else = S->getElse())
            if (!isa<CompoundStmt>(Else) && !isa<IfStmt>(Else))
                Diagnose(*S, S->getElseLoc(), "else", Else);
        return;
    }
    if (const auto* S = Result.Nodes.getNodeAs<ForStmt>("for")) {
        if (!isInAnalyzedCode(*S, S->getForLoc(), *Result.Context)) return;
        if (!isa<CompoundStmt>(S->getBody()))
            Diagnose(*S, S->getForLoc(), "for", S->getBody());
        return;
    }
    if (const auto* S = Result.Nodes.getNodeAs<CXXForRangeStmt>("range_for")) {
        if (!isInAnalyzedCode(*S, S->getForLoc(), *Result.Context)) return;
        if (!isa<CompoundStmt>(S->getBody()))
            Diagnose(*S, S->getForLoc(), "for", S->getBody());
        return;
    }
    if (const auto* S = Result.Nodes.getNodeAs<WhileStmt>("while")) {
        if (!isInAnalyzedCode(*S, S->getWhileLoc(), *Result.Context)) return;
        if (!isa<CompoundStmt>(S->getBody()))
            Diagnose(*S, S->getWhileLoc(), "while", S->getBody());
        return;
    }
    if (const auto* S = Result.Nodes.getNodeAs<DoStmt>("do")) {
        if (!isInAnalyzedCode(*S, S->getDoLoc(), *Result.Context)) return;
        if (!isa<CompoundStmt>(S->getBody()))
            Diagnose(*S, S->getDoLoc(), "do", S->getBody());
        return;
    }
    if (const auto* S = Result.Nodes.getNodeAs<SwitchStmt>("switch")) {
        if (!isInAnalyzedCode(*S, S->getSwitchLoc(), *Result.Context)) return;
        if (!isa<CompoundStmt>(S->getBody()))
            Diagnose(*S, S->getSwitchLoc(), "switch", S->getBody());
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
