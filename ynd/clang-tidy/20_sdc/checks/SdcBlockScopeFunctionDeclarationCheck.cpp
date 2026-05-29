#include "SdcBlockScopeFunctionDeclarationCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcBlockScopeFunctionDeclarationCheck::SdcBlockScopeFunctionDeclarationCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcBlockScopeFunctionDeclarationCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    functionDecl(
                        unless(isExpansionInSystemHeader()),
                        hasParent(declStmt(hasAncestor(compoundStmt()))))
                        .bind("function"),
                    this);
            }

            void SdcBlockScopeFunctionDeclarationCheck::check(const MatchFinder::MatchResult& Result) {
                const auto* Function = Result.Nodes.getNodeAs<FunctionDecl>("function");
                if (!Function) {
                    return;
                }

                diag(Function->getLocation(), "block scope function declarations shall not be used");
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
