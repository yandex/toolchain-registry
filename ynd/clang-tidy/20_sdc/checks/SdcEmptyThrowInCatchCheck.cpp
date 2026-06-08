#include "SdcEmptyThrowInCatchCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/StmtCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcEmptyThrowInCatchCheck::SdcEmptyThrowInCatchCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcEmptyThrowInCatchCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        cxxThrowExpr(unless(isExpansionInSystemHeader())).bind("throw"),
        this);
}

namespace {

// Returns true when the empty throw is syntactically inside the
// compound-statement of a catch handler.
//
// Walks the parent chain upward.  Stops at the first:
//   CXXCatchStmt → compliant (throw is inside a catch handler)
//   FunctionDecl → violation (crossed a function boundary, including a
//                  lambda's call operator, before reaching any catch handler)
//   LambdaExpr   → violation (the rule explicitly excludes lambda bodies)
bool isInsideCatchHandler(const CXXThrowExpr* TE, ASTContext& Ctx) {
    DynTypedNodeList Parents = Ctx.getParents(*TE);
    while (!Parents.empty()) {
        const DynTypedNode& P = Parents[0];

        if (P.get<CXXCatchStmt>())  return true;   // found catch handler
        if (P.get<FunctionDecl>())  return false;  // function boundary (incl. lambdas)
        if (P.get<LambdaExpr>())    return false;  // lambda body excluded explicitly

        Parents = Ctx.getParents(P);
    }
    return false;
}

} // namespace

void SdcEmptyThrowInCatchCheck::check(
    const MatchFinder::MatchResult& Result) {

    const auto* TE = Result.Nodes.getNodeAs<CXXThrowExpr>("throw");
    if (!TE) return;

    // Only flag empty throws (throw; with no operand).
    if (TE->getSubExpr() != nullptr) return;

    if (!isInsideCatchHandler(TE, *Result.Context)) {
        diag(TE->getThrowLoc(),
             "empty throw shall only occur within the compound-statement "
             "of a catch handler");
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
