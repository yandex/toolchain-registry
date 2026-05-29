#include "SdcNoCStyleFunctionalCastsCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcNoCStyleFunctionalCastsCheck::SdcNoCStyleFunctionalCastsCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoCStyleFunctionalCastsCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    cStyleCastExpr(unless(isExpansionInSystemHeader())).bind("cStyleCast"),
                    this);
                Finder->addMatcher(
                    cxxFunctionalCastExpr(unless(isExpansionInSystemHeader())).bind("functionalCast"),
                    this);
            }

            void SdcNoCStyleFunctionalCastsCheck::check(const MatchFinder::MatchResult& Result) {
                if (const auto* Cast = Result.Nodes.getNodeAs<CStyleCastExpr>("cStyleCast")) {
                    if (Cast->getType()->isVoidType()) {
                        return;
                    }

                    diag(Cast->getBeginLoc(), "C-style casts shall not be used");
                    return;
                }

                if (const auto* Cast = Result.Nodes.getNodeAs<CXXFunctionalCastExpr>("functionalCast")) {
                    if (Cast->getCastKind() == CK_ConstructorConversion || isa<InitListExpr>(Cast->getSubExpr())) {
                        return;
                    }

                    diag(Cast->getBeginLoc(), "functional notation casts shall not be used");
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
