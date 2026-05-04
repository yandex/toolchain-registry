#include "SdcNoSystemCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"
#include "llvm/ADT/ArrayRef.h"
#include <algorithm>

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcNoSystemCheck::SdcNoSystemCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoSystemCheck::registerMatchers(MatchFinder* Finder) {
                // Match calls to the system function
                Finder->addMatcher(
                    callExpr(
                        callee(functionDecl(hasName("system"))),
                        unless(isExpansionInSystemHeader()))
                        .bind("systemCall"),
                    this);

                // Match unresolved calls (when using namespace std)
                Finder->addMatcher(
                    callExpr(
                        callee(unresolvedLookupExpr(hasAnyDeclaration(namedDecl(hasName("system"))))),
                        unless(isExpansionInSystemHeader()))
                        .bind("unresolvedSystemCall"),
                    this);
            }

            void SdcNoSystemCheck::check(const MatchFinder::MatchResult& Result) {
                const auto* Call = Result.Nodes.getNodeAs<CallExpr>("systemCall");
                if (!Call) {
                    Call = Result.Nodes.getNodeAs<CallExpr>("unresolvedSystemCall");
                    if (!Call) {
                        return;
                    }
                }

                // Verify this is indeed the system function from cstdlib
                if (const FunctionDecl* FD = Call->getDirectCallee()) {
                    // Check if it's the standard system function
                    if (FD->getName() == "system") {
                        reportViolation(Call, Result);
                        return;
                    }
                } else {
                    // Handle the unresolved lookup case
                    if (const auto* Unresolved = Result.Nodes.getNodeAs<UnresolvedLookupExpr>("unresolvedSystemCall")) {
                        for (const NamedDecl* ND : Unresolved->decls()) {
                            if (ND->getName() == "system") {
                                reportViolation(Call, Result);
                                return;
                            }
                        }
                    }
                }
            }

            void SdcNoSystemCheck::reportViolation(const CallExpr* Call,
                                                     const MatchFinder::MatchResult& Result) {
                diag(Call->getBeginLoc(),
                     "library function 'system' from <cstdlib> shall not be used; "
                     "consider using platform-specific process creation APIs with proper error handling");
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
