#include "SdcForRangeInitOneCallCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcForRangeInitOneCallCheck::SdcForRangeInitOneCallCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcForRangeInitOneCallCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        cxxForRangeStmt(unless(isExpansionInSystemHeader())).bind("for"),
        this);
}

namespace {

// Counts "function calls" in an expression according to the rule:
//   - Any CallExpr (direct call, member call, overloaded operator)
//   - Any CXXConstructExpr whose result type is a class type (creates an object)
//
// Recursion descends into all sub-expressions so that chained calls and
// implicit conversions creating class-type objects are all counted.
int countCalls(const Expr* E) {
    if (!E) return 0;
    int Count = 0;

    if (isa<CallExpr>(E)) {
        // Covers CallExpr, CXXMemberCallExpr, CXXOperatorCallExpr.
        Count++;
    } else if (const auto* CE = dyn_cast<CXXConstructExpr>(E)) {
        // CXXTemporaryObjectExpr is a subclass; covers both.
        // Only count constructions that produce a class-type object — they
        // represent "creating an object of class type" per the rule.
        if (CE->getType()->isRecordType())
            Count++;
    }

    for (const Stmt* Child : E->children()) {
        if (const auto* ChildExpr = dyn_cast_or_null<Expr>(Child))
            Count += countCalls(ChildExpr);
    }
    return Count;
}

} // namespace

void SdcForRangeInitOneCallCheck::check(
    const MatchFinder::MatchResult& Result) {

    const auto* FS = Result.Nodes.getNodeAs<CXXForRangeStmt>("for");
    if (!FS) return;

    const Expr* RangeInit = FS->getRangeInit();
    if (!RangeInit) return;

    int Calls = countCalls(RangeInit);
    if (Calls > 1) {
        diag(RangeInit->getBeginLoc(),
             "for-range initializer contains %0 function call(s); "
             "at most one is permitted")
            << Calls;
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
