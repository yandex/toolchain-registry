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
                Finder->addMatcher(
                    callExpr(
                        callee(functionDecl(hasName("::std::move"))),
                        unless(isExpansionInSystemHeader()),
                        hasAncestor(functionDecl().bind("enclosing_function"))
                    ).bind("move_call"),
                    this);
            }

            void SdcStdMoveNonConstLvalueCheck::check(const MatchFinder::MatchResult& Result) {
                const auto* MoveCall = Result.Nodes.getNodeAs<CallExpr>("move_call");
                const auto* EnclosingFunc = Result.Nodes.getNodeAs<FunctionDecl>("enclosing_function");

                if (!MoveCall || MoveCall->getNumArgs() != 1) return;

                const Expr* Arg = MoveCall->getArg(0)->IgnoreParenImpCasts();
                if (!Arg) return;

                if (Arg->isTypeDependent() || Arg->isValueDependent()) {
                    return;
                }

                ExprValueKind ValueKind = Arg->getValueKind();
                QualType ArgType = Arg->getType();

                if (ValueKind == VK_PRValue) {
                    diag(MoveCall->getBeginLoc(),
                         "redundant std::move of temporary object of type %0; "
                         "the argument to std::move shall be a non-const lvalue")
                        << ArgType; // Passes the type to %0
                    return;
                }

                if (ValueKind == VK_LValue) {
                    if (ArgType.isConstQualified()) {
                        diag(MoveCall->getBeginLoc(),
                             "std::move called on const-qualified lvalue of type %0; "
                             "the argument to std::move shall be a non-const lvalue")
                            << ArgType;

                        if (EnclosingFunc) {
                            clang::TemplateSpecializationKind TSK = EnclosingFunc->getTemplateSpecializationKind();
                            if (TSK == clang::TSK_ImplicitInstantiation || TSK == clang::TSK_ExplicitInstantiationDefinition) {

                                clang::SourceLocation InstLoc = EnclosingFunc->getPointOfInstantiation();
                                if (InstLoc.isValid()) {
                                    diag(InstLoc, "in instantiation of template %0 requested here", DiagnosticIDs::Note)
                                        << EnclosingFunc;
                                }
                            }
                        }
                        return;
                    }
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
