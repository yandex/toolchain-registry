#include "SdcInitializeBasesCheck.h"
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
const CXXBaseSpecifier *baseSpecifier(const CXXConstructorDecl *Ctor, QualType Type, ASTContext &C) {
    for (const auto &B : Ctor->getParent()->bases())
        if (C.hasSameType(B.getType(), Type)) return &B;
    for (const auto &B : Ctor->getParent()->vbases())
        if (C.hasSameType(B.getType(), Type)) return &B;
    return nullptr;
}
} // namespace
void SdcInitializeBasesCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(cxxConstructorDecl(unless(isImplicit())).bind("ctor"), this);
}

void SdcInitializeBasesCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *Ctor = Result.Nodes.getNodeAs<CXXConstructorDecl>("ctor");
    if (!Ctor || !Ctor->isUserProvided() || !Ctor->doesThisDeclarationHaveABody() || Ctor->isDelegatingConstructor()) return;
    const auto L = Ctor->getBeginLoc();
    if (!isInAnalyzedCode(*Ctor, L, C) || !shouldReportPolicyDiagnostic(L, *Result.SourceManager)) return;
    llvm::SmallVector<QualType, 4> Missing;
    std::string Names;
    for (const auto *I : Ctor->inits()) {
        if (!I->isBaseInitializer() || I->isWritten() || emptyBase(I->getBaseClass()->getAsCXXRecordDecl())) continue;
        const QualType Type(I->getBaseClass(), 0);
        Missing.push_back(Type);
        if (!Names.empty()) Names += ", ";
        Names += policyTypeName(Type, C);
    }
    if (Missing.empty()) return;
    for (const Decl *Instance : Instances.claim(*Ctor, L, C)) {
        if (!diagnoseAnalysisInstance(*this, Instance, C, L,
                "constructor '%0' must explicitly initialize %select{base|bases}1 %2",
                Ctor->getQualifiedNameAsString(), unsigned(Missing.size() != 1), Names)) continue;
        for (const auto Type : Missing) {
            const auto *B = baseSpecifier(Ctor, Type, C);
            if (!B) continue;
            const auto *R = Type->getAsCXXRecordDecl();
            diag(B->getBeginLoc(), "%select{immediate|virtual}0 base %1 is declared here%2", DiagnosticIDs::Note)
                << unsigned(B->isVirtual()) << Type
                << (R && R->isPolymorphic() ? "; virtual functions exclude it from the empty-base exception" : "");
        }
    }
}
} // namespace clang::tidy::sdc
