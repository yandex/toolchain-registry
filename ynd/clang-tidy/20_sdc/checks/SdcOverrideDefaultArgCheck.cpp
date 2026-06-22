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

// Returns true if two default-argument expressions are syntactically identical
// (after canonical type substitution).  This is the right comparison for this
// rule: targets inconsistent defaults, not non-constant ones, and two
// declarations that look different in source are confusing even if they happen
// to evaluate to the same value.
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

            // Compare syntactically: the source-visible expression must be the
            // same in both declarations so that readers see consistent defaults
            // regardless of which declaration they look at.
            if (!defaultExpressionsEqual(DerivedParam->getDefaultArg(),
                                         BaseParam->getDefaultArg(), Ctx)) {
                diag(DerivedParam->getDefaultArgRange().getBegin(),
                     "default argument of overriding parameter '%0' differs "
                     "from the base class default")
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
