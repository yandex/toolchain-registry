#include "SdcSymmetricalOperatorsCheck.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
void SdcSymmetricalOperatorsCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(cxxMethodDecl(unless(isImplicit())).bind("method"), this);
}

void SdcSymmetricalOperatorsCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *M = Result.Nodes.getNodeAs<CXXMethodDecl>("method");
    // Instantiating a containing class also visits its dormant member-template
    // pattern. Only the concrete member specialization is a reportable instance.
    if (!M || M->isStatic() || M->getNumParams() != 1 || M->getDescribedFunctionTemplate()) return;
    switch (M->getOverloadedOperator()) {
    case OO_Plus: case OO_Minus: case OO_Star: case OO_Slash: case OO_Percent:
    case OO_EqualEqual: case OO_ExclaimEqual: case OO_Less: case OO_LessEqual:
    case OO_Greater: case OO_GreaterEqual: case OO_Caret: case OO_Amp: case OO_Pipe:
    case OO_AmpAmp: case OO_PipePipe: break;
    default: return;
    }
    const auto L = M->getBeginLoc();
    if (!isInAnalyzedCode(*M, L, C) || !shouldReportPolicyDiagnostic(L, *Result.SourceManager) ||
        !Reported.insert(M->getCanonicalDecl()).second) return;
    for (const Decl *Instance : Instances.claim(*M, L, C))
        diagnoseAnalysisInstance(*this, Instance, C, L,
            "implement binary operator '%0' as a non-member function; a hidden friend is permitted",
            M->getQualifiedNameAsString());
}
} // namespace clang::tidy::sdc
