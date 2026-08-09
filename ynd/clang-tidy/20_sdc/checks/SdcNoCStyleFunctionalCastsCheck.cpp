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

            bool SdcNoCStyleFunctionalCastsCheck::getDiagnosticLocations(
                SourceLocation CastLocation, const SourceManager& SM,
                SourceLocation& PrimaryLocation,
                SourceLocation& ExpansionLocation) {
                PrimaryLocation = CastLocation;
                ExpansionLocation = SourceLocation();
                if (!CastLocation.isMacroID()) {
                    return true;
                }

                // Point at the token the developer must edit. For a cast in a
                // macro body this is the definition; for a cast written as a
                // macro argument it remains the argument at the call site.
                PrimaryLocation = SM.getSpellingLoc(CastLocation);
                if (!PrimaryLocation.isValid() ||
                    SM.isInSystemHeader(PrimaryLocation)) {
                    return false;
                }

                // One macro definition or argument may produce several AST
                // cast nodes through repeated or nested expansion. Emit one
                // primary warning at the single user-written token.
                if (!ReportedMacroSpellingLocations
                         .insert(PrimaryLocation.getRawEncoding())
                         .second) {
                    return false;
                }

                ExpansionLocation = SM.getExpansionLoc(CastLocation);
                return true;
            }

            void SdcNoCStyleFunctionalCastsCheck::check(const MatchFinder::MatchResult& Result) {
                if (const auto* Cast = Result.Nodes.getNodeAs<CStyleCastExpr>("cStyleCast")) {
                    if (Cast->getType()->isVoidType()) {
                        return;
                    }

                    QualType From = Cast->getSubExpr()->IgnoreParenImpCasts()->getType();
                    QualType To = Cast->getTypeAsWritten();
                    SourceLocation Primary;
                    SourceLocation Expansion;
                    if (!getDiagnosticLocations(Cast->getBeginLoc(),
                                                *Result.SourceManager,
                                                Primary, Expansion)) {
                        return;
                    }
                    diag(Primary,
                         "C-style cast from %0 to %1 shall not be used")
                        << From << To;
                    if (Expansion.isValid() && Expansion != Primary) {
                        diag(Expansion, "cast is produced by this macro expansion",
                             DiagnosticIDs::Note);
                    }
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

                    SourceLocation Primary;
                    SourceLocation Expansion;
                    if (!getDiagnosticLocations(Cast->getBeginLoc(),
                                                *Result.SourceManager,
                                                Primary, Expansion)) {
                        return;
                    }

                    // Discarding a value to void has a permitted spelling, but
                    // only via the C-style form `(void)expr` (or a named cast);
                    // the functional form `void(expr)` is not exempt. Point the
                    // user at the compliant alternative explicitly, since it is
                    // the one case where the fix is a C-style cast rather than
                    // a static_cast.
                    if (Cast->getType()->isVoidType()) {
                        diag(Primary,
                             "functional-notation cast of %0 to void shall not be "
                             "used; use '(void)expr' to discard a value")
                            << From;
                        if (Expansion.isValid() && Expansion != Primary) {
                            diag(Expansion,
                                 "cast is produced by this macro expansion",
                                 DiagnosticIDs::Note);
                        }
                        return;
                    }

                    diag(Primary,
                         "functional-notation cast from %0 to %1 shall not be used")
                        << From << To;
                    if (Expansion.isValid() && Expansion != Primary) {
                        diag(Expansion, "cast is produced by this macro expansion",
                             DiagnosticIDs::Note);
                    }
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
