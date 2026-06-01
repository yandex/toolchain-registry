#include "SdcFunctionToPointerContextCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                // Walk through transparent wrappers (parens, full-expression
                // wrappers, constant-expression wrappers) to find the first
                // semantically-meaningful parent of `N`. We deliberately do
                // *not* walk through ImplicitCastExpr - an outer implicit cast
                // means the function pointer is being further converted, and
                // that is exactly what we want to flag.
                DynTypedNode meaningfulParent(DynTypedNode N, ASTContext& Ctx) {
                    while (true) {
                        auto Parents = Ctx.getParents(N);
                        if (Parents.empty()) return DynTypedNode();
                        DynTypedNode P = Parents[0];
                        if (P.get<ParenExpr>() ||
                            P.get<ConstantExpr>() ||
                            P.get<FullExpr>()) {
                            N = P;
                            continue;
                        }
                        return P;
                    }
                }

            } // namespace

            SdcFunctionToPointerContextCheck::
                SdcFunctionToPointerContextCheck(StringRef Name,
                                                  ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcFunctionToPointerContextCheck::registerMatchers(
                MatchFinder* Finder) {
                Finder->addMatcher(
                    implicitCastExpr(unless(isExpansionInSystemHeader()),
                                     hasCastKind(CK_FunctionToPointerDecay))
                        .bind("cast"),
                    this);
            }

            void SdcFunctionToPointerContextCheck::check(
                const MatchFinder::MatchResult& Result) {
                const auto* CE =
                    Result.Nodes.getNodeAs<ImplicitCastExpr>("cast");
                if (!CE) return;
                ASTContext& Ctx = *Result.Context;
                DynTypedNode P =
                    meaningfulParent(DynTypedNode::create(*CE), Ctx);

                // Allowed parent shapes:
                //   * static_cast (and only static_cast - the rule explicitly
                //     names this form; other explicit casts are caught by
                //     sdc-no-function-pointer-casts);
                //   * VarDecl - initialization of a pointer-to-function var,
                //     since the var's type must match the cast destination;
                //   * BinaryOperator with isAssignmentOp() - assignment to a
                //     pointer-to-function lvalue;
                //   * CallExpr / CXXConstructExpr - passing as an argument
                //     where the parameter is pointer-to-function;
                //   * ReturnStmt - returning a pointer-to-function value.
                if (P.get<CXXStaticCastExpr>()) return;
                if (P.get<VarDecl>()) return;
                if (const auto* BO = P.get<BinaryOperator>()) {
                    if (BO->isAssignmentOp()) return;
                }
                if (P.get<CallExpr>()) return;
                if (P.get<CXXConstructExpr>()) return;
                if (P.get<ReturnStmt>()) return;

                diag(CE->getExprLoc(),
                     "implicit conversion to pointer-to-function is only "
                     "permitted through a static_cast or when assigning to "
                     "an object with pointer-to-function type");
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
