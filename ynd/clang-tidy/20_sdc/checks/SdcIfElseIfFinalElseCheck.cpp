#include "SdcIfElseIfFinalElseCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcIfElseIfFinalElseCheck::SdcIfElseIfFinalElseCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcIfElseIfFinalElseCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        ifStmt(unless(isExpansionInSystemHeader())).bind("if"),
        this);
}

namespace {

// Returns true when IS is the direct else-clause of a parent IfStmt, i.e. it
// is an "else if" branch rather than the root of a new if-statement.
bool isElseIfClause(const IfStmt* IS, ASTContext& Ctx) {
    DynTypedNodeList Parents = Ctx.getParents(*IS);
    if (Parents.empty()) return false;
    const auto* ParentIf = Parents[0].get<IfStmt>();
    if (!ParentIf) return false;
    return ParentIf->getElse() == IS;
}

} // namespace

void SdcIfElseIfFinalElseCheck::check(
    const MatchFinder::MatchResult& Result) {

    const auto* IS = Result.Nodes.getNodeAs<IfStmt>("if");
    if (!IS) return;

    // Only process the root of an if-else-if chain; skip inner else-if nodes
    // to avoid reporting the same chain multiple times.
    if (isElseIfClause(IS, *Result.Context)) return;

    // Walk the else chain.
    bool hasElseIf = false;
    const IfStmt* Current = IS;

    while (const Stmt* ElseClause = Current->getElse()) {
        if (const auto* ElseIf = dyn_cast<IfStmt>(ElseClause)) {
            // This else clause is itself an if — it is an "else if".
            hasElseIf = true;
            Current = ElseIf;
        } else {
            // The else clause is a non-if statement — the chain is properly
            // terminated with a final else.
            return;
        }
    }

    // If there was at least one else-if but no terminal else: violation.
    if (hasElseIf) {
        diag(IS->getIfLoc(),
             "if-else-if chain shall be terminated with a final else statement");
    }
    // Simple if with no else (hasElseIf == false): compliant, no diagnostic.
}

} // namespace sdc
} // namespace tidy
} // namespace clang
