#include "SdcPolicyDiagnostic.h"
#include "SdcNoUnsignedUnaryMinusCheck.h"
#include "SdcCodeSelection.h"

#include "clang/AST/Expr.h"
#include "clang/AST/Decl.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcNoUnsignedUnaryMinusCheck::SdcNoUnsignedUnaryMinusCheck(
    StringRef Name, ClangTidyContext* Context)
    : SdcPolicyCheck(Name, Context) {}

void SdcNoUnsignedUnaryMinusCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        unaryOperator(hasOperatorName("-"),
                      unless(isExpansionInSystemHeader()))
            .bind("operator"),
        this);
}

void SdcNoUnsignedUnaryMinusCheck::check(
    const MatchFinder::MatchResult& Result) {
    const auto* Operator = Result.Nodes.getNodeAs<UnaryOperator>("operator");
    if (!Operator || Operator->isTypeDependent() ||
        Operator->isValueDependent()) {
        return;
    }

    const Expr* Operand = Operator->getSubExpr()->IgnoreParenImpCasts();
    QualType OperandType = Operand->getType();
    QualType NumericType = OperandType;
    if (const auto* Enum = OperandType->getAs<EnumType>()) {
        // An implementation-selected unsigned representation does not make a
        // symbolic enum an unsigned operand. Preserve explicitly unsigned enums.
        if (!Enum->getDecl()->isFixed() || Enum->getDecl()->isScoped()) return;
        NumericType = Enum->getDecl()->getIntegerType();
    }
    if (NumericType->isBooleanType() || !NumericType->isUnsignedIntegerType()) {
        return;
    }

    std::string OperandName = policyTypeName(OperandType, *Result.Context);
    if (OperandType->isEnumeralType())
        OperandName += " with unsigned underlying type " +
                       policyTypeName(NumericType, *Result.Context);

    const SourceManager& SM = *Result.SourceManager;
    SourceLocation Location = SM.getSpellingLoc(Operator->getOperatorLoc());
    if (!isInAnalyzedCode(*Operator, Operator->getOperatorLoc(),
                          *Result.Context)) {
        return;
    }

    for (const Decl* Instance : AnalysisInstances.claim(
             *Operator, Operator->getOperatorLoc(), *Result.Context)) {

        diagnoseAnalysisInstance(*this, Instance, *Result.Context, Location,
             "built-in unary '-' operator should not be applied to an unsigned "
             "expression of type %0 (result type %1)",
             OperandName,
             policyTypeName(Operator->getType(), *Result.Context));
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
