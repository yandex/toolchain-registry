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
                // In an uninstantiated template, operand types can be
                // dependent (e.g. `static_cast<T>(const T2 other)`).
                // Qualification removal cannot be determined until the
                // template is instantiated with concrete types; defer to
                // those instantiations to avoid false positives.
                if (From->isDependentType() || To->isDependentType()) {
                    return;
                }
                // 'auto' whose deduction is still pending (e.g. `const auto`
                // in a template where the initialiser is type-dependent)
                // appears as an AutoType, not a dependent type.  Skip it —
                // the check will run again on the concrete instantiation.
                if (isa<AutoType>(From.getTypePtr()) ||
                    isa<AutoType>(To.getTypePtr())) {
                    return;
                }
                auto Removal = cast_utils::findCvRemoval(From, To);
                if (!Removal) {
                    return;
                }

                StringRef Qual = (Removal->LostConst && Removal->LostVolatile)
                                     ? "const volatile"
                                 : Removal->LostConst ? "const"
                                                      : "volatile";
                diag(Cast->getBeginLoc(),
                     "cast from %0 to %1 removes '%2' qualifier; casts shall "
                     "not remove const or volatile qualification")
                    << From << To << Qual;
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
