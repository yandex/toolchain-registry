#include "SdcFuncTryBlockMemberRefCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/StmtCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcFuncTryBlockMemberRefCheck::SdcFuncTryBlockMemberRefCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcFuncTryBlockMemberRefCheck::registerMatchers(MatchFinder* Finder) {
    // Match definitions of constructors and destructors (not implicit ones).
    Finder->addMatcher(
        cxxMethodDecl(
            isDefinition(),
            unless(isImplicit()),
            unless(isExpansionInSystemHeader()),
            anyOf(cxxConstructorDecl(), cxxDestructorDecl())
        ).bind("method"),
        this);
}

namespace {

// Returns true if the member's declaring class is CD or a base of CD.
bool memberBelongsToClassOrBase(const CXXRecordDecl* MemberParent,
                                const CXXRecordDecl* CD) {
    if (!MemberParent || !CD) return false;
    if (MemberParent->getCanonicalDecl() == CD->getCanonicalDecl()) return true;
    return CD->isDerivedFrom(MemberParent);
}

// Returns true when the MemberExpr's base (after stripping casts) is the
// implicit or explicit 'this' pointer of the enclosing constructor/destructor.
bool baseIsThis(const MemberExpr* ME) {
    if (ME->isImplicitAccess()) return true;
    const Expr* Base = ME->getBase()->IgnoreImpCasts();
    return isa<CXXThisExpr>(Base);
}

// Recursively scan S (a catch-handler statement or any of its descendants)
// for references to non-static members that belong to CD or its bases.
// Reports diagnostics via Check.
void scanForMemberRefs(const Stmt* S, const CXXRecordDecl* CD,
                       ClangTidyCheck* Check) {
    if (!S) return;

    if (const auto* ME = dyn_cast<MemberExpr>(S)) {
        if (baseIsThis(ME)) {
            const ValueDecl* Member = ME->getMemberDecl();

            // Non-static data member (FieldDecl is always non-static).
            if (const auto* FD = dyn_cast<FieldDecl>(Member)) {
                const auto* Parent =
                    dyn_cast_or_null<CXXRecordDecl>(FD->getParent());
                if (memberBelongsToClassOrBase(Parent, CD)) {
                    Check->diag(ME->getMemberLoc(),
                                "catch handler of function-try-block refers to "
                                "non-static data member '%0'; the member may be "
                                "in an indeterminate state")
                        << FD->getName();
                    return; // Don't recurse further from this expression
                }
            }

            // Non-static member function call.
            if (const auto* MD = dyn_cast<CXXMethodDecl>(Member)) {
                if (!MD->isStatic() &&
                    !isa<CXXConstructorDecl>(MD) &&
                    !isa<CXXDestructorDecl>(MD)) {
                    const CXXRecordDecl* Parent = MD->getParent();
                    if (memberBelongsToClassOrBase(Parent, CD)) {
                        Check->diag(ME->getMemberLoc(),
                                    "catch handler of function-try-block calls "
                                    "non-static member function '%0'; the object "
                                    "may be in an indeterminate state")
                            << MD->getName();
                        return;
                    }
                }
            }
        }
    }

    for (const Stmt* Child : S->children())
        scanForMemberRefs(Child, CD, Check);
}

} // namespace

void SdcFuncTryBlockMemberRefCheck::check(
    const MatchFinder::MatchResult& Result) {

    const auto* Method = Result.Nodes.getNodeAs<CXXMethodDecl>("method");
    if (!Method) return;

    // A function-try-block has a CXXTryStmt as the direct body of the
    // constructor/destructor.  A regular try INSIDE the body would be wrapped
    // in a CompoundStmt first.
    const auto* TryBlock = dyn_cast_or_null<CXXTryStmt>(Method->getBody());
    if (!TryBlock) return;

    const CXXRecordDecl* CD = Method->getParent();
    if (!CD) return;

    for (unsigned i = 0; i < TryBlock->getNumHandlers(); ++i) {
        const CXXCatchStmt* Handler = TryBlock->getHandler(i);
        scanForMemberRefs(Handler->getHandlerBlock(), CD, this);
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
