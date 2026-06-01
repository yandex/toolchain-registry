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

                    QualType From = Cast->getSubExpr()->IgnoreParenImpCasts()->getType();
                    QualType To = Cast->getTypeAsWritten();
                    diag(Cast->getBeginLoc(),
                         "C-style cast from %0 to %1 shall not be used")
                        << From << To;
                    return;
                }

                if (const auto* Cast = Result.Nodes.getNodeAs<CXXFunctionalCastExpr>("functionalCast")) {
                    if (Cast->getCastKind() == CK_ConstructorConversion) {
                        return;
                    }

                    // In an uninstantiated template pattern the cast kind is
                    // CK_Dependent because the target's constructor set
                    // depends on the template arguments. Defer judgement to
                    // the instantiation, where the kind resolves (typically
                    // to CK_ConstructorConversion).
                    if (Cast->getCastKind() == CK_Dependent) {
                        return;
                    }

                    // Brace-initialization form `T{...}` is exempt by the
                    // rule (e.g. `auto j = int8_t{42};`).
                    if (Cast->isListInitialization()) {
                        return;
                    }

                    QualType From = Cast->getSubExpr()->IgnoreParenImpCasts()->getType();
                    QualType To = Cast->getTypeAsWritten();
                    diag(Cast->getBeginLoc(),
                         "functional-notation cast from %0 to %1 shall not be used")
                        << From << To;
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
