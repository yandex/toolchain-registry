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
                // When a non-type template parameter of enumeration type is
                // substituted at instantiation, clang synthesizes a
                // CStyleCastExpr (integer literal -> enum) carrying the source
                // location of the parameter use. That cast is compiler-
                // generated, not written by the user, so exclude casts whose
                // parent is a SubstNonTypeTemplateParmExpr.
                Finder->addMatcher(
                    cStyleCastExpr(unless(isExpansionInSystemHeader()),
                                   unless(hasParent(substNonTypeTemplateParmExpr())))
                        .bind("cStyleCast"),
                    this);
                Finder->addMatcher(
                    cxxFunctionalCastExpr(unless(isExpansionInSystemHeader()),
                                          unless(hasParent(substNonTypeTemplateParmExpr())))
                        .bind("functionalCast"),
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

                    // A functional-notation expression that actually invokes a
                    // constructor `T(args)` is object construction, not a
                    // type-pun cast. Most such cases carry
                    // CK_ConstructorConversion (handled above), but when the
                    // sole argument itself needs construction/conversion clang
                    // models the node as CK_NoOp wrapping a CXXConstructExpr
                    // (e.g. `SimplePolygon({{...}, ...})`). Detect the
                    // underlying construct directly so these are not flagged.
                    if (isa<CXXConstructExpr>(Cast->getSubExpr()->IgnoreImplicit())) {
                        return;
                    }

                    QualType From = Cast->getSubExpr()->IgnoreParenImpCasts()->getType();
                    QualType To = Cast->getTypeAsWritten();

                    // Discarding a value to void has a permitted spelling, but
                    // only via the C-style form `(void)expr` (or a named cast);
                    // the functional form `void(expr)` is not exempt. Point the
                    // user at the compliant alternative explicitly, since it is
                    // the one case where the fix is a C-style cast rather than
                    // a static_cast.
                    if (Cast->getType()->isVoidType()) {
                        diag(Cast->getBeginLoc(),
                             "functional-notation cast of %0 to void shall not be "
                             "used; use '(void)expr' to discard a value")
                            << From;
                        return;
                    }

                    diag(Cast->getBeginLoc(),
                         "functional-notation cast from %0 to %1 shall not be used")
                        << From << To;
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
