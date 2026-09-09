#include "SdcInitializeVariablesCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "SdcDeclarationUtils.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
using namespace declaration_detail;
void SdcInitializeVariablesCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcInitializeVariablesCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto *V = dyn_cast<VarDecl>(D);
    SourceLocation L = D->getLocation();
    StringRef Message;
    if (V && !isa<ParmVarDecl>(V) && V->isThisDeclarationADefinition() &&
        V->hasLocalStorage() && !V->hasInit() && !classType(V->getType(), C) &&
        !V->isExceptionVariable())
        Message = "initialize this variable in its definition";
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
