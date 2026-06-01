#include "SdcNoImplicitThisCaptureCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                // Decidable definition of "transient": the lambda's lifetime
                // is bounded within the enclosing full-expression by purely
                // syntactic means - no need to look across translation units
                // or inspect what a callee does.
                //
                //   (1) IIFE: the lambda is the object operand of an
                //       immediately invoked operator() call (i.e. `lam()`
                //       written at the source level).
                //   (2) Discarded: the lambda is the operand of a cast to
                //       void.
                //
                // Anything else - including passing as a function-call
                // argument - cannot be decidably proven transient and is
                // treated as non-transient.
                // Strip parens, implicit casts, and temporary-materialization
                // wrappers to reach the underlying LambdaExpr (if any).
                const Expr* peelToLambda(const Expr* E) {
                    if (!E) return nullptr;
                    E = E->IgnoreParenImpCasts();
                    while (const auto* MT =
                               dyn_cast<MaterializeTemporaryExpr>(E)) {
                        E = MT->getSubExpr()->IgnoreParenImpCasts();
                    }
                    return E;
                }

                bool isTransientLambda(const LambdaExpr* LE, ASTContext& Ctx) {
                    DynTypedNode N = DynTypedNode::create(*LE);
                    while (true) {
                        auto Parents = Ctx.getParents(N);
                        if (Parents.empty()) return false;
                        const DynTypedNode& P = Parents[0];
                        if (P.get<MaterializeTemporaryExpr>() ||
                            P.get<CXXBindTemporaryExpr>() ||
                            P.get<ImplicitCastExpr>() ||
                            P.get<ParenExpr>() ||
                            P.get<ConstantExpr>() ||
                            P.get<FullExpr>()) {
                            N = P;
                            continue;
                        }
                        // IIFE, resolved form: a `CXXOperatorCallExpr` with
                        // OO_Call where operand 0 is our lambda (operand 0 is
                        // the object; the callee is the conversion to
                        // `operator()`).
                        if (const auto* OC =
                                P.get<CXXOperatorCallExpr>()) {
                            if (OC->getOperator() == OO_Call &&
                                OC->getNumArgs() >= 1) {
                                if (peelToLambda(OC->getArg(0)) == LE) {
                                    return true;
                                }
                            }
                            return false;
                        }
                        // IIFE, dependent form: when the lambda is generic
                        // and/or sits inside a template, the call to it is
                        // type-dependent and clang cannot yet resolve
                        // `operator()`. It is then represented as a plain
                        // `CallExpr` whose *callee* is the lambda itself (e.g.
                        // SDC_VERIFY_STREAM's `[&](auto&&...){...}(expr, fn)`).
                        if (const auto* CE = P.get<CallExpr>()) {
                            if (peelToLambda(CE->getCallee()) == LE) {
                                return true;
                            }
                            return false;
                        }
                        // Discard via cast-to-void.
                        if (const auto* CC = P.get<CStyleCastExpr>()) {
                            if (CC->getType()->isVoidType()) return true;
                        }
                        if (const auto* FC =
                                P.get<CXXFunctionalCastExpr>()) {
                            if (FC->getType()->isVoidType()) return true;
                        }
                        return false;
                    }
                }

            } // namespace

            SdcNoImplicitThisCaptureCheck::SdcNoImplicitThisCaptureCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoImplicitThisCaptureCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    lambdaExpr(unless(isExpansionInSystemHeader()))
                        .bind("lambda"),
                    this);
            }

            void SdcNoImplicitThisCaptureCheck::check(
                const MatchFinder::MatchResult& Result) {
                const auto* LE = Result.Nodes.getNodeAs<LambdaExpr>("lambda");
                if (!LE) return;
                // No implicit capture mode -> rule does not apply.
                if (LE->getCaptureDefault() == LCD_None) return;

                const LambdaCapture* OffendingCap = nullptr;
                for (const LambdaCapture& Cap : LE->captures()) {
                    if (Cap.isImplicit() && Cap.capturesThis()) {
                        OffendingCap = &Cap;
                        break;
                    }
                }
                if (!OffendingCap) return;

                if (isTransientLambda(LE, *Result.Context)) return;

                diag(LE->getBeginLoc(),
                     "non-transient lambda must not implicitly capture "
                     "'this'; capture it explicitly with [this] or [*this] "
                     "and review the lifetime");
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
