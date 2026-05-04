#include "SdcNoMemFunctionsCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcNoMemFunctionsCheck::SdcNoMemFunctionsCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoMemFunctionsCheck::registerMatchers(MatchFinder* Finder) {
                // Match calls to the prohibited memory functions
                Finder->addMatcher(
                    callExpr(
                        callee(functionDecl(hasAnyName("memcpy", "memmove", "memcmp"))),
                        unless(isExpansionInSystemHeader()))
                        .bind("memFunctionCall"),
                    this);

                // Match unresolved calls (when using namespace std)
                Finder->addMatcher(
                    callExpr(
                        callee(unresolvedLookupExpr(hasAnyDeclaration(namedDecl(hasAnyName("memcpy", "memmove", "memcmp"))))),
                        unless(isExpansionInSystemHeader()))
                        .bind("unresolvedMemFunctionCall"),
                    this);
            }

            void SdcNoMemFunctionsCheck::check(const MatchFinder::MatchResult& Result) {
                const auto* Call = Result.Nodes.getNodeAs<CallExpr>("memFunctionCall");
                if (!Call) {
                    return;
                }

                // Get the function name
                StringRef FunctionName;
                if (const FunctionDecl* FD = Call->getDirectCallee()) {
                    FunctionName = FD->getName();
                } else {
                    // Handle indirect calls (should be rare for these functions)
                    return;
                }

                // Suggest alternatives based on the function
                std::string Suggestion;
                if (FunctionName == "memcpy" || FunctionName == "memmove") {
                    Suggestion = "; consider using type-safe alternatives like std::copy, assignment operators, or manual loops";
                } else if (FunctionName == "memcmp") {
                    Suggestion = "; consider using type-safe alternatives like comparison operators, std::equal, or manual element-wise comparison";
                }

                // Report the violation
                diag(Call->getBeginLoc(),
                     "The C++ Standard Library function '%0' shall not be used%1")
                    << FunctionName << Suggestion;
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
