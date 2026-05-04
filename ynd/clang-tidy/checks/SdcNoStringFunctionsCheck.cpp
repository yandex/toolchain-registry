#include "SdcNoStringFunctionsCheck.h"
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
            static const StringRef ProhibitedStringFunctions[] = {
                // cstring functions
                "strcat", "strchr", "strcmp", "strcoll", "strcpy", "strcspn",
                "strerror", "strlen", "strncat", "strncmp", "strncpy", "strpbrk",
                "strrchr", "strspn", "strstr", "strtok", "strxfrm",

                // cstdlib functions
                "strtod", "strtof", "strtol", "strtold", "strtoll", "strtoul", "strtoull",

                // cwchar functions
                "fgetwc", "fputwc", "wcstod", "wcstof", "wcstol", "wcstold",
                "wcstoll", "wcstoul", "wcstoull",

                // cinttypes functions
                "strtoimax", "strtoumax", "wcstoimax", "wcstoumax"};

            SdcNoStringFunctionsCheck::SdcNoStringFunctionsCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoStringFunctionsCheck::registerMatchers(MatchFinder* Finder) {
                // Use the static array - safe because it has program lifetime
                ArrayRef<StringRef> functions(ProhibitedStringFunctions);

                // Match calls to the prohibited string functions
                Finder->addMatcher(
                    callExpr(
                        callee(functionDecl(hasAnyName(functions))),
                        unless(isExpansionInSystemHeader()))
                        .bind("stringFunctionCall"),
                    this);

                // Match unresolved calls (when using namespace std)
                Finder->addMatcher(
                    callExpr(
                        callee(unresolvedLookupExpr(hasAnyDeclaration(namedDecl(hasAnyName(functions))))),
                        unless(isExpansionInSystemHeader()))
                        .bind("unresolvedStringFunctionCall"),
                    this);
            }

            void SdcNoStringFunctionsCheck::check(const MatchFinder::MatchResult& Result) {
                const auto* Call = Result.Nodes.getNodeAs<CallExpr>("stringFunctionCall");
                if (!Call) {
                    Call = Result.Nodes.getNodeAs<CallExpr>("unresolvedStringFunctionCall");
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
                    if (const auto* Unresolved = Result.Nodes.getNodeAs<UnresolvedLookupExpr>("unresolvedStringFunctionCall")) {
                        for (const NamedDecl* ND : Unresolved->decls()) {
                            FunctionName = ND->getName();
                            // Check if it's one of our prohibited functions using the static list
                            ArrayRef<StringRef> prohibitedFunctions(ProhibitedStringFunctions);

                            // Check if function name matches any prohibited function
                            bool found = false;
                            for (StringRef prohibited : prohibitedFunctions) {
                                if (FunctionName == prohibited) {
                                    found = true;
                                    break;
                                }
                            }

                            if (found) {
                                break;
                            }
                        }
                    }
                }

                // Report the violation with appropriate error message
                std::string Suggestion;
                if (FunctionName.starts_with("strto") || FunctionName.starts_with("wcsto")) {
                    Suggestion = "; consider using std::sto* functions with proper error handling";
                } else if (FunctionName.starts_with("str") || FunctionName.starts_with("wc")) {
                    Suggestion = "; consider using std::string or std::wstring with their member functions";
                } else {
                    Suggestion = "; consider using C++ alternatives with proper safety mechanisms";
                }

                diag(Call->getBeginLoc(),
                     "The string handling function '%0' shall not be used%1")
                    << FunctionName << Suggestion;
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
