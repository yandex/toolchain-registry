#include "SdcNoFunctionalCastStatementCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcNoFunctionalCastStatementCheck::SdcNoFunctionalCastStatementCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcNoFunctionalCastStatementCheck::registerMatchers(MatchFinder* Finder) {
    // Match explicit type conversions using functional notation:
    //   CXXFunctionalCastExpr  — scalar types:   int(x), float(y)
    //   CXXTemporaryObjectExpr — class types:    MyClass(args), MyClass{args}
    Finder->addMatcher(
        expr(
            anyOf(cxxFunctionalCastExpr(), cxxTemporaryObjectExpr()),
            unless(isExpansionInSystemHeader())
        ).bind("cast"),
        this);
}

namespace {

// Returns true when the expression is used as a standalone statement inside a
// compound statement (block).
//
// Clang inserts transparent wrapper nodes between the expression and the
// CompoundStmt: ExprWithCleanups (for destructor calls), CXXBindTemporaryExpr
// (destructor binding), MaterializeTemporaryExpr, and ParenExpr.  We walk
// through only these specific wrappers.  Any other parent — including
// meaningful expressions like CallExpr, BinaryOperator, ReturnStmt — means
// the expression is being *used*, not discarded as a statement.
bool isExpressionStatement(const Expr* E, ASTContext& Ctx) {
    DynTypedNodeList Parents = Ctx.getParents(*E);
    while (!Parents.empty()) {
        const DynTypedNode& P = Parents[0];
        if (P.get<CompoundStmt>()) return true;
        // Transparent wrappers inserted by Clang for cleanup/binding.
        if (P.get<ExprWithCleanups>()        ||
            P.get<CXXBindTemporaryExpr>()    ||
            P.get<MaterializeTemporaryExpr>() ||
            P.get<ParenExpr>()) {
            Parents = Ctx.getParents(P);
            continue;
        }
        return false; // any other parent means the expression is being used
    }
    return false;
}

} // namespace

void SdcNoFunctionalCastStatementCheck::check(
    const MatchFinder::MatchResult& Result) {

    const auto* E = Result.Nodes.getNodeAs<Expr>("cast");
    if (!E) return;

    if (!isExpressionStatement(E, *Result.Context)) return;

    diag(E->getBeginLoc(),
         "explicit type conversion using functional notation shall not be "
         "an expression statement; the converted object is immediately "
         "destroyed");
}

} // namespace sdc
} // namespace tidy
} // namespace clang
