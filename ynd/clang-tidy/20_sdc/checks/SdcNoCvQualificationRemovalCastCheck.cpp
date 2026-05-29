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
                if (!cast_utils::removesCvQualification(From, To)) {
                    return;
                }

                diag(Cast->getBeginLoc(),
                     "casts shall not remove const or volatile qualification from the type accessed via a pointer or reference");
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
