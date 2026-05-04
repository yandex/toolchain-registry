#include "SdcNoStaticLocalVariablesCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Type.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcNoStaticLocalVariablesCheck::SdcNoStaticLocalVariablesCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoStaticLocalVariablesCheck::registerMatchers(
                MatchFinder* Finder) {
                // Match all variable declarations with static storage duration
                // We'll filter for local variables and const/constexpr in the check() method
                Finder->addMatcher(
                    varDecl(
                        hasStaticStorageDuration(),
                        unless(isExpansionInSystemHeader()))
                        .bind("static_var"),
                    this);
            }

            void SdcNoStaticLocalVariablesCheck::check(
                const MatchFinder::MatchResult& Result) {
                const auto* VarDecl = Result.Nodes.getNodeAs<clang::VarDecl>("static_var");

                if (!VarDecl) {
                    return;
                }

                // Check if this is a local variable (not a global, class member, or namespace variable)
                if (!isLocalVariable(VarDecl, Result)) {
                    return;
                }

                // Exempt constexpr variables
                if (VarDecl->isConstexpr()) {
                    return;
                }

                // Exempt const variables
                QualType Type = VarDecl->getType();
                if (Type.isConstQualified()) {
                    return;
                }

                // If we get here, it's a static local variable that is not const or constexpr
                diag(VarDecl->getBeginLoc(),
                     "local variables shall not have static storage duration");
            }

            bool SdcNoStaticLocalVariablesCheck::isLocalVariable(
                const clang::VarDecl* VarDecl,
                const MatchFinder::MatchResult& Result) {
                if (!VarDecl) {
                    return false;
                }

                // Check if it's a class static member - these are not local variables
                if (VarDecl->isStaticDataMember()) {
                    return false;
                }

                // Get the declaration context
                const DeclContext* DC = VarDecl->getDeclContext();

                if (!DC) {
                    return false;
                }

                // Local variables are declared inside functions, methods, lambdas, or blocks
                // Check if the variable is declared in a function-like context
                if (DC->isFunctionOrMethod()) {
                    return true;
                }

                // Check if it's in a block (C blocks or lambdas)
                if (const auto* BlockDecl = clang::dyn_cast<clang::BlockDecl>(DC)) {
                    return true;
                }

                // Not a local variable if declared at namespace scope (including global scope)
                // or at class scope
                return false;
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
