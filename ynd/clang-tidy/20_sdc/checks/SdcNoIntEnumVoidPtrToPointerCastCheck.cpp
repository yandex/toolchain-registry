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

                StringRef SourceKind;
                if (cast_utils::isVoidPointer(From)) {
                    SourceKind = "void pointer";
                } else if (From->isEnumeralType()) {
                    SourceKind = "enumeration";
                } else {
                    SourceKind = "integral";
                }

                diag(Cast->getBeginLoc(),
                     "cast from %0 type %1 to pointer type %2 is not permitted")
                    << SourceKind << From << To;
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
