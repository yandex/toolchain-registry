#include "SdcNoCvQualificationRemovalCastCheck.h"

#include "SdcCastUtils.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcNoCvQualificationRemovalCastCheck::SdcNoCvQualificationRemovalCastCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoCvQualificationRemovalCastCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    explicitCastExpr(unless(isExpansionInSystemHeader())).bind("cast"),
                    this);
            }

            void SdcNoCvQualificationRemovalCastCheck::check(const MatchFinder::MatchResult& Result) {
                const auto* Cast = Result.Nodes.getNodeAs<ExplicitCastExpr>("cast");
                if (!Cast || Cast->getTypeAsWritten()->isVoidType()) {
                    return;
                }

                QualType From = Cast->getSubExpr()->getType();
                QualType To = Cast->getTypeAsWritten();
                auto Removal = cast_utils::findCvRemoval(From, To);
                if (!Removal) {
                    return;
                }

                StringRef Qual = (Removal->LostConst && Removal->LostVolatile)
                                     ? "const volatile"
                                 : Removal->LostConst ? "const"
                                                      : "volatile";
                diag(Cast->getBeginLoc(),
                     "cast removes '%0' qualifier from %1; casts shall not "
                     "remove const or volatile qualification")
                    << Qual << Removal->QualifiedType;
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
