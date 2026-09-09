#include "SdcConsistentNoreturnCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/Attr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
void SdcConsistentNoreturnCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(functionDecl(unless(isImplicit())).bind("function"), this);
}
void SdcConsistentNoreturnCheck::check(const MatchFinder::MatchResult &Result) {
    const auto *F = Result.Nodes.getNodeAs<FunctionDecl>("function");
    auto &C = *Result.Context;
    if (!F || F->isInvalidDecl() || !isInAnalyzedCode(*F, F->getLocation(), C)) return;
    // Template attribute provenance needs a separate model; a cloned attribute
    // must not be mistaken for a written declaration of this specialization.
    if (F->getTemplatedKind() != FunctionDecl::TK_NonTemplate) return;
    // An inherited AST attribute is evidence of the other declaration, not
    // evidence that this declaration explicitly carried the attribute.
    const CXX11NoReturnAttr *Evidence = nullptr;
    for (const auto *D : F->redecls()) {
        if (D->isInvalidDecl()) return;
        for (const auto *A : D->specific_attrs<CXX11NoReturnAttr>()) {
            if (D == F && !A->isInherited()) return;
            if (!A->isInherited()) Evidence = A;
        }
    }
    if (!Evidence) return;
    for (const Decl *Instance : Instances.claim(*F, F->getLocation(), C)) {
        diagnoseAnalysisInstance(*this, Instance, C, F->getLocation(),
            "repeat the noreturn attribute on this declaration");
        diag(Evidence->getLocation(), "another declaration explicitly specifies noreturn here", DiagnosticIDs::Note);
    }
}
} // namespace clang::tidy::sdc
