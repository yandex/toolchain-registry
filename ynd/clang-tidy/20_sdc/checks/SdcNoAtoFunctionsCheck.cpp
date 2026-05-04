#include "SdcNoAtoFunctionsCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcNoAtoFunctionsCheck::SdcNoAtoFunctionsCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoAtoFunctionsCheck::registerMatchers(MatchFinder* Finder) {
                // Match calls to the prohibited ato* functions
                Finder->addMatcher(
                    callExpr(
                        callee(functionDecl(hasAnyName("atof", "atoi", "atol", "atoll"))),
                        unless(isExpansionInSystemHeader()))
                        .bind("atoFunctionCall"),
                    this);

                // Match unresolved calls (when using namespace std)
                Finder->addMatcher(
                    callExpr(
                        callee(unresolvedLookupExpr(hasAnyDeclaration(namedDecl(hasAnyName("atof", "atoi", "atol", "atoll"))))),
                        unless(isExpansionInSystemHeader()))
                        .bind("unresolvedAtoFunctionCall"),
                    this);
            }

            void SdcNoAtoFunctionsCheck::check(const MatchFinder::MatchResult& Result) {
                const auto* Call = Result.Nodes.getNodeAs<CallExpr>("atoFunctionCall");
                if (!Call) {
                    Call = Result.Nodes.getNodeAs<CallExpr>("unresolvedAtoFunctionCall");
                    if (!Call) {
                        return;
                    }
                }

                // Get the function name
                StringRef FunctionName;
                if (const FunctionDecl* FD = Call->getDirectCallee()) {
                    FunctionName = FD->getName();
                } else {
                    // Handle the unresolved lookup case
                    if (const auto* Unresolved = Result.Nodes.getNodeAs<UnresolvedLookupExpr>("unresolvedAtoFunctionCall")) {
                        for (const NamedDecl* ND : Unresolved->decls()) {
                            FunctionName = ND->getName();
                            if (FunctionName == "atof" || FunctionName == "atoi" ||
                                FunctionName == "atol" || FunctionName == "atoll") {
                                break;
                            }
                        }
                    }
                }

                // Report the violation with appropriate error message
                std::string Suggestion;
                if (FunctionName == "atoi") {
                    Suggestion = "; consider using std::stoi with proper error handling";
                } else if (FunctionName == "atol") {
                    Suggestion = "; consider using std::stol with proper error handling";
                } else if (FunctionName == "atof") {
                    Suggestion = "; consider using std::stod with proper error handling";
                } else if (FunctionName == "atoll") {
                    Suggestion = "; consider using std::stoll with proper error handling";
                }

                diag(Call->getBeginLoc(),
                     "library function '%0' from <cstdlib> shall not be used%1")
                    << FunctionName << Suggestion;
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
