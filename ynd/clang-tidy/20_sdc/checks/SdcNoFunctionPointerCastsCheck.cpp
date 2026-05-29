#include "SdcNoFunctionPointerCastsCheck.h"

#include "SdcCastUtils.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {
            namespace {

                bool isPermittedStandardOrUserDefinedConversion(CastKind Kind) {
                    return Kind == CK_FunctionToPointerDecay || Kind == CK_NoOp ||
                           Kind == CK_NullToPointer || Kind == CK_UserDefinedConversion;
                }

                bool isPermittedVoidDiscard(const ExplicitCastExpr* Cast) {
                    return Cast->getTypeAsWritten()->isVoidType() &&
                           cast_utils::isFunctionOrMemberPointer(Cast->getSubExpr()->IgnoreParenImpCasts()->getType()) &&
                           isa<CallExpr>(Cast->getSubExpr()->IgnoreParenImpCasts());
                }

            } // namespace

            SdcNoFunctionPointerCastsCheck::SdcNoFunctionPointerCastsCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoFunctionPointerCastsCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    explicitCastExpr(unless(isExpansionInSystemHeader())).bind("cast"),
                    this);
            }

            void SdcNoFunctionPointerCastsCheck::check(const MatchFinder::MatchResult& Result) {
                const auto* Cast = Result.Nodes.getNodeAs<ExplicitCastExpr>("cast");
                if (!Cast) {
                    return;
                }

                QualType From = Cast->getSubExpr()->IgnoreParenImpCasts()->getType();
                QualType To = Cast->getTypeAsWritten();
                bool FromFunctionPointer = cast_utils::isFunctionOrMemberPointer(From);
                bool ToFunctionPointer = cast_utils::isFunctionOrMemberPointer(To);

                if (!FromFunctionPointer && !ToFunctionPointer) {
                    return;
                }

                if (isPermittedVoidDiscard(Cast)) {
                    return;
                }

                if (ToFunctionPointer && isPermittedStandardOrUserDefinedConversion(Cast->getCastKind())) {
                    return;
                }

                diag(Cast->getBeginLoc(), "casts shall not be performed between a pointer to function and any other type");
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
