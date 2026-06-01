#include "SdcNoThrowStackAddressCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcNoThrowStackAddressCheck::SdcNoThrowStackAddressCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoThrowStackAddressCheck::registerMatchers(MatchFinder* Finder) {
                // Match `throw <addr-of-local>` in any form. The inner pattern
                // is:
                //   * an address-of `&x` whose operand resolves to a
                //     non-reference VarDecl with automatic storage duration; or
                //   * a DeclRefExpr to a pointer-typed local variable - we let
                //     check() walk through implicit casts / parens.
                //
                // We intentionally do NOT inspect whether the throw is caught
                // we flag every throw that could leak a stack address.
                Finder->addMatcher(
                    cxxThrowExpr(
                        unless(isExpansionInSystemHeader()),
                        hasDescendant(
                            unaryOperator(
                                hasOperatorName("&"),
                                hasUnaryOperand(
                                    ignoringParenImpCasts(
                                        declRefExpr(
                                            to(varDecl(
                                                hasAutomaticStorageDuration(),
                                                unless(hasType(referenceType())))
                                                .bind("var"))))))
                                .bind("addrof")))
                        .bind("throw"),
                    this);
            }

            void SdcNoThrowStackAddressCheck::check(
                const MatchFinder::MatchResult& Result) {
                const auto* Throw = Result.Nodes.getNodeAs<CXXThrowExpr>("throw");
                const auto* AddrOf = Result.Nodes.getNodeAs<UnaryOperator>("addrof");
                const auto* Var = Result.Nodes.getNodeAs<VarDecl>("var");
                if (!Throw || !AddrOf || !Var) {
                    return;
                }

                // The matcher pinpoints the inner `&x`; report at the throw
                // keyword so the diagnostic is anchored to the offending
                // statement, with a note at the address-of subexpression.
                diag(Throw->getThrowLoc(),
                     "throw expression carries the address of an automatic "
                     "local variable; the address becomes dangling once the "
                     "exception leaves the function")
                    << Throw->getSourceRange();
                diag(AddrOf->getOperatorLoc(),
                     "address-of automatic variable %0 captured here",
                     DiagnosticIDs::Note)
                    << Var;
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
