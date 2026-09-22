#include "SdcCopyMoveSignaturesCheck.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
void SdcCopyMoveSignaturesCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(cxxMethodDecl(unless(isImplicit())).bind("method"), this);
}

void SdcCopyMoveSignaturesCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *M = Result.Nodes.getNodeAs<CXXMethodDecl>("method");
    const auto *Ctor = dyn_cast_or_null<CXXConstructorDecl>(M);
    if (!M || !M->isUserProvided() ||
        !((Ctor && Ctor->isCopyOrMoveConstructor()) ||
          M->isCopyAssignmentOperator() || M->isMoveAssignmentOperator())) return;
    const auto L = M->getBeginLoc();
    if (!isInAnalyzedCode(*M, L, C) || !shouldReportPolicyDiagnostic(L, *Result.SourceManager)) return;
    const bool Move = (Ctor && Ctor->isMoveConstructor()) || M->isMoveAssignmentOperator();
    const QualType Self = C.getRecordType(M->getParent());
    const QualType Param = Move ? C.getRValueReferenceType(Self) : C.getLValueReferenceType(Self.withConst());
    std::string Problems;
    auto Add = [&](const std::string &Problem) {
        if (!Problems.empty()) Problems += "; ";
        Problems += Problem;
    };
    if (M->getNumParams() != 1) Add("expected exactly one parameter, found " + std::to_string(M->getNumParams()));
    if (!C.hasSameType(M->getParamDecl(0)->getType(), Param))
        Add("parameter type is " + policyTypeName(M->getParamDecl(0)->getType(), C) +
            "; expected " + policyTypeName(Param, C));
    if (M->isVirtual()) Add("must not be virtual");
    if (M->isVariadic()) Add("must not be variadic");
    if (!Ctor) {
        if (M->getRefQualifier() != RQ_LValue) Add("must be lvalue-ref-qualified with '&'");
        if (M->getMethodQualifiers().hasQualifiers()) Add("must not be const- or volatile-qualified");
        if (!M->getReturnType()->isVoidType() && !C.hasSameType(M->getReturnType(), C.getLValueReferenceType(Self)))
            Add("return type is " + policyTypeName(M->getReturnType(), C) +
                "; expected " + policyTypeName(C.getLValueReferenceType(Self), C) + " or 'void'");
    }
    if (Move && !M->getType()->castAs<FunctionProtoType>()->isNothrow())
        Add("must have a non-throwing noexcept specification");
    if (Problems.empty() || !Reported.insert(M->getCanonicalDecl()).second) return;
    for (const Decl *Instance : Instances.claim(*M, L, C))
        diagnoseAnalysisInstance(*this, Instance, C, L,
            "%select{copy|move}0 %select{assignment operator|constructor}1 '%2': %3",
            unsigned(Move), unsigned(Ctor != nullptr), M->getQualifiedNameAsString(), Problems);
}
} // namespace clang::tidy::sdc
