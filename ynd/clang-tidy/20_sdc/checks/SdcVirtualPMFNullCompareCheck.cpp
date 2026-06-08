#include "SdcVirtualPMFNullCompareCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcVirtualPMFNullCompareCheck::SdcVirtualPMFNullCompareCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

static bool isNullptrExpr(const Expr* E) {
    return isa<CXXNullPtrLiteralExpr>(E);
}

// A PMF is potentially virtual if:
//   case 1 – it is &Class::method and method is virtual
//   case 2 – the class is incomplete (can't see its vtable layout)
//   case 3 – it is a non-constant PMF whose type matches any virtual method of the class
static bool isPotentiallyVirtualPMF(const Expr* E, ASTContext& Ctx) {
    // Case 1: &Class::method
    if (const auto* UO = dyn_cast<UnaryOperator>(E)) {
        if (UO->getOpcode() == UO_AddrOf) {
            const Expr* Sub = UO->getSubExpr()->IgnoreParenImpCasts();
            if (const auto* DRE = dyn_cast<DeclRefExpr>(Sub)) {
                if (const auto* MD = dyn_cast<CXXMethodDecl>(DRE->getDecl()))
                    return MD->isVirtual();
            }
        }
    }

    // Cases 2 & 3: inspect the class via the MemberPointerType
    const auto* MPT = E->getType()->getAs<MemberPointerType>();
    if (!MPT) return false;

    const CXXRecordDecl* RD = MPT->getClass()->getAsCXXRecordDecl();
    // Incomplete class — can't rule out virtual members (case 2)
    if (!RD || !RD->isCompleteDefinition()) return true;

    // Case 3: any virtual method of the class has the same signature as the PMF
    QualType FuncTy = MPT->getPointeeType();
    for (const CXXMethodDecl* M : RD->methods()) {
        if (!M->isVirtual()) continue;
        if (Ctx.hasSameType(M->getType(), FuncTy)) return true;
    }

    return false;
}

void SdcVirtualPMFNullCompareCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        binaryOperator(
            anyOf(hasOperatorName("=="), hasOperatorName("!=")),
            hasEitherOperand(expr(hasType(memberPointerType())))
        ).bind("cmp"),
        this
    );
}

void SdcVirtualPMFNullCompareCheck::check(const MatchFinder::MatchResult& Result) {
    const auto* BO = Result.Nodes.getNodeAs<BinaryOperator>("cmp");

    const Expr* LHS = BO->getLHS()->IgnoreParenImpCasts();
    const Expr* RHS = BO->getRHS()->IgnoreParenImpCasts();

    // Comparing with nullptr is always compliant
    if (isNullptrExpr(LHS) || isNullptrExpr(RHS)) return;

    ASTContext* Ctx = Result.Context;

    auto potVirt = [&](const Expr* E) -> bool {
        return E->getType()->isMemberFunctionPointerType() &&
               isPotentiallyVirtualPMF(E, *Ctx);
    };

    if (!potVirt(LHS) && !potVirt(RHS)) return;

    diag(BO->getOperatorLoc(),
         "comparison of potentially virtual pointer-to-member-function "
         "shall only be with nullptr");
}

} // namespace sdc
} // namespace tidy
} // namespace clang
