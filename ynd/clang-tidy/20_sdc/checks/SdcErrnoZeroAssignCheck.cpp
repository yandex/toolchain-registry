#include "SdcPolicyDiagnostic.h"
#include "SdcErrnoZeroAssignCheck.h"

#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcErrnoZeroAssignCheck::SdcErrnoZeroAssignCheck(StringRef Name,
                                                   ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcErrnoZeroAssignCheck::registerMatchers(MatchFinder* Finder) {
    // Match all assignment operators that are not in system headers — errno
    // assignments inside the stdlib are permitted.  Compound assignments are
    // not the literal-token assignment required by the rule, so they are
    // diagnosed unconditionally once the LHS is errno.
    Finder->addMatcher(
        binaryOperator(
            isAssignmentOperator(),
            unless(isExpansionInSystemHeader())
        ).bind("assign"),
        this
    );
}

static bool hasSystemDeclaration(const VarDecl& Variable,
                                 const SourceManager& SM) {
    for (const VarDecl* Redeclaration : Variable.redecls()) {
        if (SM.isInSystemHeader(
                SM.getSpellingLoc(Redeclaration->getLocation())))
            return true;
    }
    return false;
}

static bool isErrnoExpr(const Expr* E, const SourceManager& SM,
                        const LangOptions& LO) {
    // Case 1: errno is a plain extern variable — direct DeclRefExpr.
    if (const auto* DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenCasts())) {
        if (const auto* Variable = dyn_cast<VarDecl>(DRE->getDecl())) {
            if (Variable->getQualifiedNameAsString() == "errno" &&
                hasSystemDeclaration(*Variable, SM)) {
                return true;
            }
        }
    }

    // Case 2: errno is a macro (e.g. (*__errno_location())) — the begin
    // location of the expanded expression lies inside the errno macro body.
    // Requiring the ultimately written token to be in a system header avoids
    // confusing a project-defined macro with the Standard Library facility.
    SourceLocation Loc = E->getBeginLoc();
    if (Loc.isMacroID() &&
        Lexer::getImmediateMacroName(Loc, SM, LO) == "errno") {
        return SM.isInSystemHeader(SM.getSpellingLoc(Loc));
    }

    return false;
}

void SdcErrnoZeroAssignCheck::check(const MatchFinder::MatchResult& Result) {
    const auto* BO = Result.Nodes.getNodeAs<BinaryOperator>("assign");

    if (!isErrnoExpr(BO->getLHS(), *Result.SourceManager,
                     Result.Context->getLangOpts()))
        return;

    const auto Instances = AnalysisInstances.claim(
        *BO, BO->getOperatorLoc(), *Result.Context);
    if (Instances.empty()) {
        return;
    }

    if (BO->getOpcode() != BO_Assign) {
        for (const Decl* Instance : Instances) {

            diagnoseAnalysisInstance(*this, Instance, *Result.Context, BO->getOperatorLoc(),
                 "compound assignment to 'errno' is not allowed; only the "
                 "literal value zero may be assigned to 'errno'");
        }
        return;
    }

    // RHS must be the integer literal 0 (after stripping implicit casts and
    // parens).  Macros that expand to the token '0' (e.g. #define OK 0) are
    // already resolved to IntegerLiteral(0) in the AST, so they pass.
    // Variables, named constants, and non-zero values do not.
    const Expr* RHS = BO->getRHS()->IgnoreParenImpCasts();
    if (const auto* IL = dyn_cast<IntegerLiteral>(RHS))
        if (IL->getValue().isZero()) return;

    for (const Decl* Instance : Instances) {

        diagnoseAnalysisInstance(*this, Instance, *Result.Context, BO->getOperatorLoc(),
             "only the literal value zero may be assigned to 'errno'");
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
