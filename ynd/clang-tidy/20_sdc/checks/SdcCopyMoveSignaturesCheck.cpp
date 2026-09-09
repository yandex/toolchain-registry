#include "SdcCopyMoveSignaturesCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
void SdcCopyMoveSignaturesCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcCopyMoveSignaturesCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto *M = dyn_cast<CXXMethodDecl>(D);
    const auto *Ctor = dyn_cast<CXXConstructorDecl>(D);
    SourceLocation L = D->getLocation();
    StringRef Message;
    if (M && M->isUserProvided() &&
        ((Ctor && Ctor->isCopyOrMoveConstructor()) || M->isCopyAssignmentOperator() || M->isMoveAssignmentOperator())) {
        bool Move = (Ctor && Ctor->isMoveConstructor()) || M->isMoveAssignmentOperator();
        QualType Self = C.getRecordType(M->getParent());
        QualType Param = Move ? C.getRValueReferenceType(Self) : C.getLValueReferenceType(Self.withConst());
        bool Bad = M->getNumParams() != 1 || !C.hasSameType(M->getParamDecl(0)->getType(), Param) ||
                   M->isVirtual() || M->isVariadic();
        if (!Ctor) Bad |= M->getRefQualifier() != RQ_LValue || M->getMethodQualifiers().hasQualifiers() ||
            (!M->getReturnType()->isVoidType() && !C.hasSameType(M->getReturnType(), C.getLValueReferenceType(Self)));
        if (Move) Bad |= !M->getType()->castAs<FunctionProtoType>()->isNothrow();
        if (Bad) Message = "use the prescribed copy/move signature, ref-qualification and noexcept specification";
    }
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
