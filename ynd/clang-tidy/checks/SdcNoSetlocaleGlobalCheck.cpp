#include "SdcNoSetlocaleGlobalCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcNoSetlocaleGlobalCheck::SdcNoSetlocaleGlobalCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoSetlocaleGlobalCheck::registerMatchers(MatchFinder* Finder) {
                // Match calls to setlocale function (both C and C++ variants)
                Finder->addMatcher(
                    callExpr(
                        callee(functionDecl(hasName("setlocale"))),
                        unless(isExpansionInSystemHeader()))
                        .bind("setlocale_call"),
                    this);

                // Match calls to std::setlocale (qualified version)
                Finder->addMatcher(
                    callExpr(
                        callee(functionDecl(hasName("setlocale"), hasParent(namespaceDecl(hasName("std"))))),
                        unless(isExpansionInSystemHeader()))
                        .bind("std_setlocale_call"),
                    this);

                // Match calls to std::locale::global - use a simpler approach
                // Static member functions can be matched by their qualified name
                Finder->addMatcher(
                    callExpr(
                        callee(functionDecl(hasName("std::locale::global"))),
                        unless(isExpansionInSystemHeader()))
                        .bind("locale_global_call"),
                    this);

                // Also match setlocale when called through using namespace std using unresolvedLookupExpr
                Finder->addMatcher(
                    callExpr(
                        callee(unresolvedLookupExpr(hasAnyDeclaration(namedDecl(hasName("setlocale"))))),
                        unless(isExpansionInSystemHeader()))
                        .bind("unresolved_setlocale_call"),
                    this);
            }

            void SdcNoSetlocaleGlobalCheck::check(
                const MatchFinder::MatchResult& Result) {
                // Check for setlocale calls (both direct and std::)
                if (const auto* SetlocaleCall = Result.Nodes.getNodeAs<clang::CallExpr>("setlocale_call")) {
                    diag(SetlocaleCall->getBeginLoc(),
                         "the setlocale function shall not be called");
                    return;
                }

                // Check for unresolved setlocale calls (like when using namespace std)
                if (const auto* UnresolvedCall = Result.Nodes.getNodeAs<clang::CallExpr>("unresolved_setlocale_call")) {
                    diag(UnresolvedCall->getBeginLoc(),
                         "the setlocale function shall not be called");
                    return;
                }

                // Check for std::locale::global calls
                if (const auto* LocaleGlobalCall = Result.Nodes.getNodeAs<clang::CallExpr>("locale_global_call")) {
                    diag(LocaleGlobalCall->getBeginLoc(),
                         "the std::locale::global function shall not be called");
                    return;
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
