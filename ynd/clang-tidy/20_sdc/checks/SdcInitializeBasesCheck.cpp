#include "SdcInitializeBasesCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
namespace {
bool emptyBase(const CXXRecordDecl *R) {
    if (!R || !(R = R->getDefinition()) || !R->field_empty() ||
        R->isPolymorphic() || R->getNumVBases()) return false;
    for (const auto &B : R->bases())
        if (!emptyBase(B.getType()->getAsCXXRecordDecl())) return false;
    return true;
}
} // namespace
void SdcInitializeBasesCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcInitializeBasesCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto *Ctor = dyn_cast<CXXConstructorDecl>(D);
    const auto *R = dyn_cast<CXXRecordDecl>(D);
    SourceLocation L = D->getLocation();
    StringRef Message;
    if (Ctor && Ctor->isUserProvided() && Ctor->doesThisDeclarationHaveABody() && !Ctor->isDelegatingConstructor()) {
        for (const auto *I : Ctor->inits())
            if (I->isBaseInitializer() && !I->isWritten() &&
                !emptyBase(I->getBaseClass()->getAsCXXRecordDecl()))
                Message = "explicitly initialize every non-empty immediate and virtual base class";
    }
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
