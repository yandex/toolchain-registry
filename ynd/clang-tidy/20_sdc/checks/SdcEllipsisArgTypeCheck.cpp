#include "SdcEllipsisArgTypeCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprConcepts.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcEllipsisArgTypeCheck::SdcEllipsisArgTypeCheck(StringRef Name,
                                                   ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

namespace {

// requiresExpr() exists in upstream LLVM docs and was added to ASTMatchers.h
// in a later release; the Ubuntu 20.1.8 package header omits it despite the
// compiled library already traversing RequiresExpr nodes.  Define it manually
// using the same VariadicDynCastAllOfMatcher pattern as every other node
// matcher — this is the correct and future-compatible approach.
const internal::VariadicDynCastAllOfMatcher<Stmt, RequiresExpr> requiresExpr;

} // namespace

void SdcEllipsisArgTypeCheck::registerMatchers(MatchFinder* Finder) {
    // Exclude calls in all standard unevaluated contexts:
    //   sizeof/alignof  → unaryExprOrTypeTraitExpr()
    //   noexcept(expr)  → cxxNoexceptExpr()
    //   typeid(expr)    → cxxTypeidExpr()  [no built-in matcher; see handler]
    //   requires{expr}  → requiresExpr()   [defined locally above]
    Finder->addMatcher(
        callExpr(
            unless(isExpansionInSystemHeader()),
            unless(hasAncestor(unaryExprOrTypeTraitExpr())),
            unless(hasAncestor(cxxNoexceptExpr())),
            unless(hasAncestor(requiresExpr()))
        ).bind("call"),
        this
    );
}

// Returns a human-readable reason if T is inappropriate for ellipsis passing,
// or nullptr if it is appropriate.
static const char* inappropriateReason(QualType T) {
    const auto* RD = T.getCanonicalType()->getAsCXXRecordDecl();
    if (!RD || !RD->isCompleteDefinition()) return nullptr;

    if (RD->isPolymorphic())
        return "virtual member functions";

    if (!RD->hasTrivialCopyConstructor() || !RD->hasTrivialMoveConstructor() ||
        !RD->hasTrivialCopyAssignment()  || !RD->hasTrivialMoveAssignment())
        return "non-trivial copy or move operations";

    if (!RD->hasTrivialDestructor())
        return "non-trivial destructor";

    return nullptr;
}

void SdcEllipsisArgTypeCheck::check(const MatchFinder::MatchResult& Result) {
    const auto* CE = Result.Nodes.getNodeAs<CallExpr>("call");

    // Resolve the callee's function prototype (handles both direct calls and
    // calls through function pointers).
    QualType CalleeT = CE->getCallee()->IgnoreParenCasts()->getType();
    if (const auto* PT = CalleeT->getAs<PointerType>())
        CalleeT = PT->getPointeeType();
    const auto* FPT = CalleeT->getAs<FunctionProtoType>();
    if (!FPT || !FPT->isVariadic()) return;

    // typeid(expr) lacks a built-in matcher in clang-20.  Its operand is a
    // direct child of CXXTypeidExpr, so a single-level parent check suffices.
    for (const auto& P : Result.Context->getParents(*CE))
        if (P.get<CXXTypeidExpr>()) return;

    const unsigned NumNamedParams = FPT->getNumParams();

    for (unsigned I = NumNamedParams; I < CE->getNumArgs(); ++I) {
        const Expr* Arg = CE->getArg(I);
        QualType T = Arg->getType();

        if (T->isDependentType()) continue;

        if (const char* Reason = inappropriateReason(T))
            diag(Arg->getExprLoc(),
                 "argument of type %0 passed via ellipsis has %1; "
                 "behaviour is undefined")
                << T << Reason;
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
