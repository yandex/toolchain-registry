#include "SdcInitializeMembersCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "SdcDeclarationUtils.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
using namespace declaration_detail;
namespace {
bool missingMember(const CXXRecordDecl *R, const CXXConstructorDecl *Ctor,
                   ASTContext &C) {
    if (Ctor && (Ctor->isDelegatingConstructor() || Ctor->isCopyOrMoveConstructor()))
        return false;
    for (const auto *F : R->fields()) {
        if (F->isUnnamedBitField() || F->hasInClassInitializer() || classType(F->getType(), C))
            continue;
        bool Initialized = false;
        if (Ctor)
            for (const auto *I : Ctor->inits())
                if (I->isMemberInitializer() && I->getMember() == F && I->isWritten())
                    Initialized = true;
        if (!Initialized) return true;
    }
    return false;
}
} // namespace
void SdcInitializeMembersCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcInitializeMembersCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto *V = dyn_cast<VarDecl>(D);
    const auto *F = dyn_cast<FunctionDecl>(D);
    const auto *Ctor = dyn_cast<CXXConstructorDecl>(D);
    const auto *R = dyn_cast<CXXRecordDecl>(D);
    SourceLocation L = D->getLocation();
    StringRef Message;
    if (Ctor && !Ctor->isDeleted() && (Ctor->doesThisDeclarationHaveABody() || Ctor->isExplicitlyDefaulted()) &&
        !Ctor->getParent()->isAggregate() && missingMember(Ctor->getParent(), Ctor, C))
        Message = "initialize every direct data member before the constructor body";
    if (R && R->isThisDeclarationADefinition() && !R->isAggregate() && !R->hasUserDeclaredConstructor() &&
        !R->isLambda() && missingMember(R, nullptr, C))
        Message = "the implicit default constructor leaves a direct data member uninitialized";
    if (V && !isa<ParmVarDecl>(V) && V->isThisDeclarationADefinition()) {
        const auto *VR = V->getType()->getAsCXXRecordDecl();
        // Clang represents implicit default initialization as a construct expression.
        bool Explicit = V->hasInit() && !(V->getInitStyle() == VarDecl::CallInit &&
            isa<CXXConstructExpr>(V->getInit()) && !cast<CXXConstructExpr>(V->getInit())->getParenOrBraceRange().isValid());
        if (VR && VR->hasDefinition() && VR->isAggregate() && !Explicit && missingMember(VR, nullptr, C))
            Message = "initialize this aggregate so that all direct data members are initialized";
    }
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
