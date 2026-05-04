#include "SdcStdMoveNonConstLvalueCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcStdMoveNonConstLvalueCheck::SdcStdMoveNonConstLvalueCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcStdMoveNonConstLvalueCheck::registerMatchers(MatchFinder* Finder) {
                // Match calls to std::move
                Finder->addMatcher(
                    callExpr(
                        callee(functionDecl(hasName("::std::move"))),
                        unless(isExpansionInSystemHeader()))
                        .bind("move_call"),
                    this);
            }

            void SdcStdMoveNonConstLvalueCheck::check(
                const MatchFinder::MatchResult& Result) {
                const auto* MoveCall = Result.Nodes.getNodeAs<CallExpr>("move_call");

                if (!MoveCall || MoveCall->getNumArgs() != 1) {
                    return;
                }

                const Expr* Arg = MoveCall->getArg(0)->IgnoreParenImpCasts();
                if (!Arg) {
                    return;
                }

                // Get the value category and type of the argument
                ExprValueKind ValueKind = Arg->getValueKind();
                QualType ArgType = Arg->getType();

                // Check if the argument is an rvalue (prvalue or xvalue)
                // If it's a prvalue (temporary), std::move is redundant
                if (ValueKind == VK_PRValue) {
                    diag(MoveCall->getBeginLoc(),
                         "redundant std::move of temporary object; "
                         "the argument to std::move shall be a non-const lvalue");
                    return;
                }

                // Now check if it's an lvalue (which is what we want)
                if (ValueKind == VK_LValue) {
                    // Check if the lvalue is const-qualified
                    if (ArgType.isConstQualified()) {
                        diag(MoveCall->getBeginLoc(),
                             "std::move called on const-qualified lvalue; "
                             "the argument to std::move shall be a non-const lvalue");
                        return;
                    }

                    // If we get here, it's a non-const lvalue, which is compliant
                    return;
                }

                // If it's an xvalue (result of another move or cast), it's technically okay
                // but we could add additional checks here if needed
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
