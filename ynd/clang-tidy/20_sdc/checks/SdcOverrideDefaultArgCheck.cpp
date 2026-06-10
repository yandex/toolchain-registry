#include "SdcOverrideDefaultArgCheck.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

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

// Returns true if the parameter has a usable (parsed, instantiated) default.
bool hasUsableDefault(const ParmVarDecl* P) {
    return P->hasDefaultArg() &&
           !P->hasUnparsedDefaultArg() &&
           !P->hasUninstantiatedDefaultArg();
}

// Returns true if two APValues represent the same value.
// Handles the common cases (integer, float); treats unknown kinds as unequal.
bool apValuesEqual(const APValue& A, const APValue& B) {
    if (A.getKind() != B.getKind()) return false;
    switch (A.getKind()) {
    case APValue::Int:
        return A.getInt() == B.getInt();
    case APValue::Float:
        return A.getFloat().bitwiseIsEqual(B.getFloat());
    case APValue::None:
    case APValue::Indeterminate:
        return true;
    default:
        return false; // conservative: treat as different
    }
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

    // Check against each directly overridden base method.
    for (const CXXMethodDecl* Base : Method->overridden_methods()) {
        const CXXMethodDecl* BaseCanon = Base->getCanonicalDecl();

        unsigned NumParams = Method->getNumParams();
        for (unsigned i = 0; i < NumParams; ++i) {
            const ParmVarDecl* DerivedParam = Method->getParamDecl(i);

            // Option 1: no default specified in the override — always compliant.
            if (!hasUsableDefault(DerivedParam)) continue;

            const Expr* DerivedExpr = DerivedParam->getDefaultArg();

            // The override's default must be a constant expression.
            Expr::EvalResult DerivedEval;
            bool DerivedIsConst =
                DerivedExpr->EvaluateAsConstantExpr(DerivedEval, Ctx);

            if (!DerivedIsConst) {
                diag(DerivedParam->getDefaultArgRange().getBegin(),
                     "default argument of overriding parameter '%0' is not a "
                     "constant expression")
                    << DerivedParam->getName();
                diag(BaseCanon->getLocation(),
                     "overrides base method declared here",
                     DiagnosticIDs::Note);
                continue;
            }

            // The base parameter must also have a default.
            if (i >= BaseCanon->getNumParams()) continue;
            const ParmVarDecl* BaseParam = BaseCanon->getParamDecl(i);

            if (!hasUsableDefault(BaseParam)) {
                diag(DerivedParam->getDefaultArgRange().getBegin(),
                     "overriding parameter '%0' specifies a default argument "
                     "but the corresponding base parameter has none")
                    << DerivedParam->getName();
                diag(BaseCanon->getLocation(),
                     "overrides base method declared here",
                     DiagnosticIDs::Note);
                continue;
            }

            // The base's default must also be a constant expression.
            const Expr* BaseExpr = BaseParam->getDefaultArg();
            Expr::EvalResult BaseEval;
            bool BaseIsConst = BaseExpr->EvaluateAsConstantExpr(BaseEval, Ctx);

            if (!BaseIsConst) {
                diag(DerivedParam->getDefaultArgRange().getBegin(),
                     "overriding parameter '%0' has a constant default argument "
                     "but the base parameter's default is not a constant expression")
                    << DerivedParam->getName();
                diag(BaseCanon->getLocation(),
                     "overrides base method declared here",
                     DiagnosticIDs::Note);
                continue;
            }

            // Both are constant: values must be equal.
            if (!apValuesEqual(DerivedEval.Val, BaseEval.Val)) {
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
