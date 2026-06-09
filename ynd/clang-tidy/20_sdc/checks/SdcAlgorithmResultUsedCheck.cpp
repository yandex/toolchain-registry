#include "SdcAlgorithmResultUsedCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcAlgorithmResultUsedCheck::SdcAlgorithmResultUsedCheck(StringRef Name,
                                                          ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

// Returns true if the expression result is discarded (not used).
// Per the rule, a cast to void is also "not used".
// Walks through: ExprWithCleanups (destructor wrapping) and void casts.
static bool isResultDiscarded(const Expr* E, ASTContext& Ctx) {
    const Expr* Current = E;
    for (;;) {
        auto Parents = Ctx.getParents(*Current);
        if (Parents.empty()) return false;

        const DynTypedNode& P = Parents[0];

        // Direct expression statement — result is dropped.
        if (P.get<CompoundStmt>()) return true;

        // ExprWithCleanups is a transparent wrapper; keep walking.
        if (const auto* EWC = P.get<ExprWithCleanups>()) {
            Current = EWC;
            continue;
        }

        // Void cast (both C-style and static_cast<void>) — explicitly discarded.
        if (const auto* Cast = P.get<CastExpr>()) {
            if (Cast->getType()->isVoidType()) {
                Current = Cast;
                continue;
            }
        }

        // Any other expression or statement parent means the value is used.
        return false;
    }
}

// Returns true when D is inside the std namespace (handles inline sub-namespaces).
static bool isInStdNamespace(const Decl* D) {
    const DeclContext* DC = D->getDeclContext()->getEnclosingNamespaceContext();
    const auto* NS = dyn_cast<NamespaceDecl>(DC);
    return NS && NS->isStdNamespace();
}

void SdcAlgorithmResultUsedCheck::registerMatchers(MatchFinder* Finder) {
    // std::remove, std::remove_if, std::unique.
    // The <cstdio> remove(const char*) is filtered in check() via getPrimaryTemplate().
    Finder->addMatcher(
        callExpr(
            callee(functionDecl(hasAnyName("remove", "remove_if", "unique"))),
            unless(isExpansionInSystemHeader())
        ).bind("algo"),
        this
    );

    // Member empty() — .empty() call on any object.
    Finder->addMatcher(
        cxxMemberCallExpr(
            callee(cxxMethodDecl(hasName("empty"))),
            unless(isExpansionInSystemHeader())
        ).bind("empty"),
        this
    );

    // Non-member std::empty() (C++17 <iterator> form).
    Finder->addMatcher(
        callExpr(
            callee(functionDecl(hasName("empty"))),
            unless(isExpansionInSystemHeader()),
            unless(cxxMemberCallExpr())
        ).bind("empty"),
        this
    );
}

void SdcAlgorithmResultUsedCheck::check(const MatchFinder::MatchResult& Result) {
    ASTContext* Ctx = Result.Context;

    if (const auto* CE = Result.Nodes.getNodeAs<CallExpr>("algo")) {
        const FunctionDecl* FD = CE->getDirectCallee();
        if (!FD || !isInStdNamespace(FD)) return;
        // Exclude <cstdio> remove(const char*): it has no primary template.
        // Algorithm remove/remove_if/unique are function templates.
        if (!FD->getPrimaryTemplate()) return;

        if (isResultDiscarded(CE, *Ctx))
            diag(CE->getBeginLoc(),
                 "the result of '%0' shall be used")
                << FD->getName();
        return;
    }

    if (const auto* MCE = Result.Nodes.getNodeAs<CXXMemberCallExpr>("empty")) {
        // Only flag std library empty() per the rule.
        const CXXMethodDecl* MD = MCE->getMethodDecl();
        if (!MD || !isInStdNamespace(MD)) return;

        if (isResultDiscarded(MCE, *Ctx))
            diag(MCE->getBeginLoc(),
                 "the result of 'empty' shall be used");
        return;
    }

    if (const auto* CE = Result.Nodes.getNodeAs<CallExpr>("empty")) {
        const FunctionDecl* FD = CE->getDirectCallee();
        if (!FD || !isInStdNamespace(FD)) return;

        if (isResultDiscarded(CE, *Ctx))
            diag(CE->getBeginLoc(),
                 "the result of 'std::empty' shall be used");
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
