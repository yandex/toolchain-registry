#include "SdcNoImplicitThisCaptureCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                // Transparent temporary wrappers do not extend a callback's
                // lifetime beyond the surrounding full-expression.
                const Expr* peelToLambda(const Expr* E) {
                    if (!E) return nullptr;
                    E = E->IgnoreParenImpCasts();
                    while (const auto* MT =
                               dyn_cast<MaterializeTemporaryExpr>(E)) {
                        E = MT->getSubExpr()->IgnoreParenImpCasts();
                    }
                    return E;
                }

                class CallbackUseAnalysis {
                    ASTContext& Ctx;
                    llvm::SmallPtrSet<const ParmVarDecl*, 16> Active;

                    bool knownSynchronous(const FunctionDecl* FD, unsigned Index) {
                        const DeclContext* DC = FD->getDeclContext();
                        while (const auto* NS = dyn_cast<NamespaceDecl>(DC)) {
                            if (!NS->isInline()) break;
                            DC = NS->getParent();
                        }
                        const auto* NS = dyn_cast<NamespaceDecl>(DC);
                        if (!NS || !NS->isStdNamespace() || !FD->getIdentifier()) return false;
                        // Standard contracts, independent of implementation
                        // adapters that may keep a local copy of the predicate.
                        return (FD->getName() == "any_of" &&
                                (FD->getNumParams() == 3 || FD->getNumParams() == 4) &&
                                Index + 1 == FD->getNumParams()) ||
                               (FD->getName() == "erase_if" && FD->getNumParams() == 2 && Index == 1);
                    }

                    struct Uses : RecursiveASTVisitor<Uses> {
                        CallbackUseAnalysis& Analysis;
                        const ParmVarDecl* Parameter;
                        bool Safe = true;
                        Uses(CallbackUseAnalysis& A, const ParmVarDecl* P) : Analysis(A), Parameter(P) {}
                        bool VisitDeclRefExpr(DeclRefExpr* E) {
                            if (E->getDecl() == Parameter && !Analysis.consumed(E)) Safe = false;
                            return Safe;
                        }
                    };

                    bool parameter(const FunctionDecl* FD, unsigned Index) {
                        if (!FD || Index >= FD->getNumParams()) return false;
                        if (const auto* Method = dyn_cast<CXXMethodDecl>(FD))
                            if (Method->isVirtual()) return false; // The runtime override may store it.
                        if (knownSynchronous(FD, Index)) return true;
                        const FunctionDecl* Definition = nullptr;
                        if (!FD->hasBody(Definition)) return false;
                        const ParmVarDecl* P = Definition->getParamDecl(Index);
                        // Cyclic forwarding needs a separate fixed-point proof.
                        if (Active.size() >= 64 || !Active.insert(P).second) return false;
                        Uses Visitor(*this, P);
                        Visitor.TraverseStmt(const_cast<Stmt*>(Definition->getBody()));
                        Active.erase(P);
                        return Visitor.Safe;
                    }

                public:
                    explicit CallbackUseAnalysis(ASTContext& C) : Ctx(C) {}
                    bool consumed(const Expr* E) {
                        DynTypedNode N = DynTypedNode::create(*E);
                        while (true) {
                            auto Parents = Ctx.getParents(N);
                            if (Parents.size() != 1) return false;
                            const auto& P = Parents[0];
                            if (P.get<ParenExpr>() || P.get<ImplicitCastExpr>() ||
                                P.get<MaterializeTemporaryExpr>() || P.get<CXXBindTemporaryExpr>() ||
                                P.get<FullExpr>()) { N = P; continue; }
                            if (const auto* Construct = P.get<CXXConstructExpr>()) {
                                if (!Construct->getConstructor()->isCopyOrMoveConstructor()) return false;
                                N = P;
                                continue;
                            }
                            if (const auto* Call = P.get<CallExpr>()) {
                                const Expr* Child = N.get<Expr>();
                                if (Call->getCallee() == Child) return true;
                                if (const auto* Op = dyn_cast<CXXOperatorCallExpr>(Call)) {
                                    if (Op->getOperator() == OO_Call && Op->getArg(0) == Child) return true;
                                }
                                const FunctionDecl* FD = Call->getDirectCallee();
                                if (!FD) return false;
                                // Moving/forwarding the parameter is transparent;
                                // its eventual consumer still must be proven safe.
                                if (FD->getIdentifier() &&
                                    (FD->getName() == "move" || FD->getName() == "forward")) {
                                    const auto* NS = dyn_cast<NamespaceDecl>(FD->getDeclContext());
                                    if (NS && NS->isStdNamespace() && Call->getNumArgs() == 1) {
                                        N = P;
                                        continue;
                                    }
                                }
                                unsigned Offset = isa<CXXOperatorCallExpr>(Call) &&
                                    isa<CXXMethodDecl>(FD) && !cast<CXXMethodDecl>(FD)->isStatic() ? 1 : 0;
                                for (unsigned I = Offset; I < Call->getNumArgs(); ++I)
                                    if (Call->getArg(I) == Child) return parameter(FD, I - Offset);
                            }
                            return false;
                        }
                    }
                };

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
                            return CallbackUseAnalysis(Ctx).consumed(LE);
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
                            return CallbackUseAnalysis(Ctx).consumed(LE);
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
