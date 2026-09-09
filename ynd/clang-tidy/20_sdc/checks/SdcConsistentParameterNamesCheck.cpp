#include "SdcConsistentParameterNamesCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
void SdcConsistentParameterNamesCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(functionDecl(unless(isImplicit())).bind("function"), this);
}
void SdcConsistentParameterNamesCheck::check(const MatchFinder::MatchResult &Result) {
    const auto *F = Result.Nodes.getNodeAs<FunctionDecl>("function");
    auto &C = *Result.Context;
    if (!F || F->isInvalidDecl()) return;
    llvm::SmallVector<const FunctionDecl *, 8> Previous;
    for (auto *P = F->getPreviousDecl(); P; P = P->getPreviousDecl()) Previous.push_back(P);
    if (const auto *M = dyn_cast<CXXMethodDecl>(F))
        for (const auto *Base : M->overridden_methods()) Previous.push_back(Base);
    for (unsigned I = 0; I < F->getNumParams(); ++I) {
        const auto *P = F->getParamDecl(I);
        if (!P->getIdentifier() || !isInAnalyzedCode(*P, P->getLocation(), C)) continue;
        for (const auto *Other : Previous) {
            if (Other->isInvalidDecl() || Other->getNumParams() != F->getNumParams()) continue;
            // Clang can link an explicit specialization to a synthesized
            // declaration instantiated from its primary template. Those
            // parameter names are not declarations of the specialization.
            if (F->getTemplateSpecializationKind() == TSK_ExplicitSpecialization &&
                Other->getTemplateSpecializationKind() != TSK_ExplicitSpecialization) continue;
            bool PrimarySpelling = false;
            if (const auto *Template = F->getPrimaryTemplate())
                for (const auto *P : Template->redecls()) {
                    const auto *Primary = cast<FunctionDecl>(P->getTemplatedDecl());
                    PrimarySpelling |= Other->getLocation() == Primary->getLocation();
                    if (Primary->getNumParams() == F->getNumParams())
                        PrimarySpelling |= Other->getParamDecl(I)->getLocation() ==
                            Primary->getParamDecl(I)->getLocation();
                }
            if (PrimarySpelling) continue;
            const auto *Q = Other->getParamDecl(I);
            if (!Q->getIdentifier() || P->getName() == Q->getName()) continue;
            for (const Decl *Instance : Instances.claim(*P, P->getLocation(), C)) {
                diagnoseAnalysisInstance(*this, Instance, C, P->getLocation(),
                    "parameter name '%0' differs from the visible declaration name '%1'",
                    P->getName(), Q->getName());
                diag(Q->getLocation(), "conflicting parameter name is written here", DiagnosticIDs::Note);
            }
            break;
        }
    }
}
} // namespace clang::tidy::sdc
