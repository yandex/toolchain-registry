#include "SdcInitializeMembersCheck.h"
#include "SdcDeclarationUtils.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
using namespace declaration_detail;
namespace {
llvm::SmallVector<const FieldDecl *, 4> missingMembers(
        const CXXRecordDecl *R, const CXXConstructorDecl *Ctor, ASTContext &C) {
    llvm::SmallVector<const FieldDecl *, 4> Missing;
    if (Ctor && (Ctor->isDelegatingConstructor() || Ctor->isCopyOrMoveConstructor())) return Missing;
    for (const auto *F : R->fields()) {
        if (F->isUnnamedBitField() || F->hasInClassInitializer() || classType(F->getType(), C)) continue;
        bool Initialized = false;
        if (Ctor)
            for (const auto *I : Ctor->inits())
                if (I->isMemberInitializer() && I->getMember() == F && I->isWritten()) Initialized = true;
        if (!Initialized) Missing.push_back(F);
    }
    return Missing;
}
} // namespace
void SdcInitializeMembersCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcInitializeMembersCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto L = D->getBeginLoc();
    if (!isInAnalyzedCode(*D, L, C) || !shouldReportPolicyDiagnostic(L, *Result.SourceManager)) return;
    llvm::SmallVector<const FieldDecl *, 4> Missing;
    std::string Subject;
    if (const auto *Ctor = dyn_cast<CXXConstructorDecl>(D)) {
        if (!Ctor->isDeleted() && (Ctor->doesThisDeclarationHaveABody() || Ctor->isExplicitlyDefaulted()) &&
            !Ctor->getParent()->isAggregate()) {
            Missing = missingMembers(Ctor->getParent(), Ctor, C);
            Subject = std::string(Ctor->isExplicitlyDefaulted() ? "defaulted constructor '" : "constructor '") +
                Ctor->getQualifiedNameAsString() + "'";
        }
    } else if (const auto *R = dyn_cast<CXXRecordDecl>(D)) {
        if (R->isThisDeclarationADefinition() && !R->isAggregate() && !R->hasUserDeclaredConstructor() && !R->isLambda()) {
            Missing = missingMembers(R, nullptr, C);
            Subject = "implicit default constructor of '" + R->getQualifiedNameAsString() + "'";
        }
    } else if (const auto *V = dyn_cast<VarDecl>(D)) {
        if (!isa<ParmVarDecl>(V) && V->isThisDeclarationADefinition()) {
            const auto *R = V->getType()->getAsCXXRecordDecl();
            // Clang represents implicit default initialization as a construct expression.
            const bool Explicit = V->hasInit() && !(V->getInitStyle() == VarDecl::CallInit &&
                isa<CXXConstructExpr>(V->getInit()) && !cast<CXXConstructExpr>(V->getInit())->getParenOrBraceRange().isValid());
            if (R && R->hasDefinition() && R->isAggregate() && !Explicit) {
                Missing = missingMembers(R, nullptr, C);
                Subject = "aggregate '" + V->getNameAsString() + "' of type " + policyTypeName(V->getType(), C);
            }
        }
    }
    if (Missing.empty()) return;
    std::string Names;
    for (const auto *F : Missing) {
        if (!Names.empty()) Names += ", ";
        Names += "'" + F->getNameAsString() + "'";
    }
    for (const Decl *Instance : Instances.claim(*D, L, C)) {
        if (!diagnoseAnalysisInstance(*this, Instance, C, L,
                "%0 must initialize %select{member|members}1 %2 before the object is accessible",
                Subject, unsigned(Missing.size() != 1), Names)) continue;
        for (const auto *F : Missing)
            diag(F->getLocation(), "member '%0' of type %1 has no default member initializer or written member initializer",
                 DiagnosticIDs::Note) << F->getName() << policyTypeName(F->getType(), C);
    }
}
} // namespace clang::tidy::sdc
