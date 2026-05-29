#include "SdcNoIntEnumVoidPtrToPointerCastCheck.h"

#include "SdcCastUtils.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {
            namespace {

                bool isPointerDestination(QualType Type) {
                    QualType Canonical = cast_utils::canonicalUnqualified(Type);
                    return Canonical->isPointerType() || Canonical->isMemberPointerType();
                }

                bool isSourceCoveredByRule(QualType Type) {
                    return cast_utils::isIntegralOrEnumeration(Type) || cast_utils::isVoidPointer(Type);
                }

                bool isFunctionPointerException(QualType Type) {
                    return cast_utils::isFunctionPointer(Type) || cast_utils::isMemberFunctionPointer(Type);
                }

                bool isVoidPointerToVoidPointer(QualType From, QualType To) {
                    return cast_utils::isVoidPointer(From) && cast_utils::isVoidPointer(To);
                }

            } // namespace

            SdcNoIntEnumVoidPtrToPointerCastCheck::SdcNoIntEnumVoidPtrToPointerCastCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoIntEnumVoidPtrToPointerCastCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    explicitCastExpr(unless(isExpansionInSystemHeader())).bind("cast"),
                    this);
            }

            void SdcNoIntEnumVoidPtrToPointerCastCheck::check(const MatchFinder::MatchResult& Result) {
                const auto* Cast = Result.Nodes.getNodeAs<ExplicitCastExpr>("cast");
                if (!Cast) {
                    return;
                }

                QualType From = Cast->getSubExpr()->IgnoreParenImpCasts()->getType();
                QualType To = Cast->getTypeAsWritten();

                if (!isPointerDestination(To) || !isSourceCoveredByRule(From)) {
                    return;
                }

                if (isFunctionPointerException(To) || isVoidPointerToVoidPointer(From, To)) {
                    return;
                }

                diag(Cast->getBeginLoc(),
                     "objects with integral, enumerated, or void pointer type shall not be cast to pointer type");
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
