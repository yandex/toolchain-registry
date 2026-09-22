#include "SdcExternalArraySizeCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
void SdcExternalArraySizeCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcExternalArraySizeCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto *V = dyn_cast<VarDecl>(D);
    SourceLocation L = D->getLocation();
    SourceLocation BoundLocation;
    StringRef Message;
    if (V && V->hasExternalFormalLinkage() && !V->isThisDeclarationADefinition() &&
        V->getTypeSourceInfo()) {
        auto A = V->getTypeSourceInfo()->getTypeLoc().getAs<ArrayTypeLoc>();
        if (A && !A.getSizeExpr()) {
            Message = "specify the size in the non-defining declaration of external array '%0'";
            BoundLocation = A.getLBracketLoc();
        }
    }
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C)) {
        for (const Decl *Instance : Instances.claim(DynTypedNode::create(*D), L, C)) {
            if (!diagnoseAnalysisInstance(*this, Instance, C, L, Message, V->getName())) continue;
            // A macro backtrace may stop at a wrapper or a template expansion.
            // Show the omitted bound itself, independently of that backtrace.
            if (BoundLocation.isMacroID())
                diag(getUltimateWrittenLocation(BoundLocation, *Result.SourceManager),
                     "array '%0' is declared without an explicit bound here",
                     DiagnosticIDs::Note) << V->getName();
        }
    }
}
} // namespace clang::tidy::sdc
