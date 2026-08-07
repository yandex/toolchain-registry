#include "SdcOverrideDefaultArgCheck.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/FoldingSet.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcOverrideDefaultArgCheck::SdcOverrideDefaultArgCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcOverrideDefaultArgCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        cxxMethodDecl(
            unless(isImplicit()),
            unless(isExpansionInSystemHeader())
        ).bind("method"),
        this);
}

namespace {

// Returns true if the parameter has a usable (parsed, instantiated) default
// whose expression can be safely profiled.
bool hasUsableDefault(const ParmVarDecl* P) {
    return P->hasDefaultArg() &&
           !P->hasUnparsedDefaultArg() &&
           !P->hasUninstantiatedDefaultArg();
}

// Project qualification decision: compare source-level expression structure,
// not evaluated values. Different spellings can convey different intent even
// when they currently evaluate to the same value.
bool defaultExpressionsEqual(const Expr* A, const Expr* B,
                             const ASTContext& Ctx) {
    llvm::FoldingSetNodeID IdA, IdB;
    A->Profile(IdA, Ctx, /*Canonical=*/true);
    B->Profile(IdB, Ctx, /*Canonical=*/true);
    return IdA == IdB;
}

} // namespace

void SdcOverrideDefaultArgCheck::check(
    const MatchFinder::MatchResult& Result) {

    const auto* Method = Result.Nodes.getNodeAs<CXXMethodDecl>("method");
    if (!Method) return;

    // Only overriding methods are relevant.
    if (Method->size_overridden_methods() == 0) return;

    // Process only the canonical declaration to avoid double-reporting when
    // an out-of-line definition is also in the TU.
    if (Method != Method->getCanonicalDecl()) return;

    ASTContext& Ctx = *Result.Context;

    for (const CXXMethodDecl* Base : Method->overridden_methods()) {
        const CXXMethodDecl* BaseCanon = Base->getCanonicalDecl();

        unsigned NumParams = Method->getNumParams();
        for (unsigned i = 0; i < NumParams; ++i) {
            const ParmVarDecl* DerivedParam = Method->getParamDecl(i);

            // No default in override — always compliant.
            if (!hasUsableDefault(DerivedParam)) continue;

            if (i >= BaseCanon->getNumParams()) continue;
            const ParmVarDecl* BaseParam = BaseCanon->getParamDecl(i);

            // Base has no default at all — violation.
            if (!BaseParam->hasDefaultArg()) {
                diag(DerivedParam->getDefaultArgRange().getBegin(),
                     "overriding parameter '%0' specifies a default argument "
                     "but the corresponding base parameter has none")
                    << DerivedParam->getName();
                diag(BaseCanon->getLocation(),
                     "overrides base method declared here",
                     DiagnosticIDs::Note);
                continue;
            }

            // Base has a default that is uninstantiated or unparsed — we
            // cannot profile it yet; defer to the instantiation.
            if (!hasUsableDefault(BaseParam)) continue;

            const Expr* DerivedDefault = DerivedParam->getDefaultArg();
            const Expr* BaseDefault = BaseParam->getDefaultArg();

            // Constant-expression validity is a separate normative condition.
            // The QM decision makes source-level identity an additional,
            // conservative condition; it does not exempt identical
            // non-constant expressions.
            if (!DerivedDefault->isCXX11ConstantExpr(Ctx) ||
                !BaseDefault->isCXX11ConstantExpr(Ctx) ||
                !defaultExpressionsEqual(DerivedDefault, BaseDefault, Ctx)) {
                diag(DerivedParam->getDefaultArgRange().getBegin(),
                     "default argument of overriding parameter '%0' and its "
                     "base default shall be constant expressions with "
                     "identical source structure")
                    << DerivedParam->getName();
                diag(BaseCanon->getLocation(),
                     "overrides base method declared here",
                     DiagnosticIDs::Note);
            }
        }
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
