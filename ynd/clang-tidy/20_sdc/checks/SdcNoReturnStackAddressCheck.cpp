#include "SdcNoReturnStackAddressCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/LambdaCapture.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                const Expr* peel(const Expr* E) {
                    return E ? E->IgnoreParenImpCasts() : nullptr;
                }

                bool isAutomaticObject(const VarDecl* VD) {
                    if (!VD) return false;
                    if (VD->getType()->isReferenceType()) return false;
                    return VD->hasLocalStorage();
                }

                const VarDecl* varFromDeclRef(const Expr* E) {
                    const auto* DRE = dyn_cast_or_null<DeclRefExpr>(peel(E));
                    if (!DRE) return nullptr;
                    return dyn_cast<VarDecl>(DRE->getDecl());
                }

                const VarDecl* findBaseAutomaticObject(const Expr* E) {
                    E = peel(E);
                    if (!E) return nullptr;

                    if (const auto* DRE = dyn_cast<DeclRefExpr>(E)) {
                        const auto* VD = dyn_cast<VarDecl>(DRE->getDecl());
                        return isAutomaticObject(VD) ? VD : nullptr;
                    }

                    if (const auto* ME = dyn_cast<MemberExpr>(E)) {
                        return findBaseAutomaticObject(ME->getBase());
                    }

                    if (const auto* ASE = dyn_cast<ArraySubscriptExpr>(E)) {
                        return findBaseAutomaticObject(ASE->getBase());
                    }

                    return nullptr;
                }

                const VarDecl* findAddressedAutomaticObject(const Expr* E) {
                    E = E ? E->IgnoreParens() : nullptr;
                    if (!E) return nullptr;

                    // &x, &x.member, &arr[0]
                    if (const auto* UO = dyn_cast<UnaryOperator>(E)) {
                        if (UO->getOpcode() == UO_AddrOf) {
                            return findBaseAutomaticObject(UO->getSubExpr());
                        }
                        return nullptr;
                    }

                    // Bare array name in pointer-return context: array-to-pointer
                    // decay still designates the automatic array.
                    const VarDecl* VD = varFromDeclRef(E);
                    if (VD && VD->getType()->isArrayType() && isAutomaticObject(VD)) {
                        return VD;
                    }

                    // std::addressof(x)
                    if (const auto* CE = dyn_cast<CallExpr>(peel(E))) {
                        const FunctionDecl* FD = CE->getDirectCallee();
                        if (!FD || !FD->getIdentifier() || FD->getName() != "addressof") {
                            return nullptr;
                        }
                        const DeclContext* DC = FD->getDeclContext();
                        while (DC && DC->isInlineNamespace()) {
                            DC = DC->getParent();
                        }
                        const auto* NS = dyn_cast_or_null<NamespaceDecl>(DC);
                        if (!NS || !NS->isStdNamespace()) {
                            return nullptr;
                        }
                        if (CE->getNumArgs() == 0) return nullptr;
                        return findBaseAutomaticObject(CE->getArg(0));
                    }

                    return nullptr;
                }

                void diagnoseAddressReturn(ClangTidyCheck& Check,
                                           const ReturnStmt* Ret,
                                           const VarDecl* VD) {
                    Check.diag(Ret->getReturnLoc(),
                               "return statement returns the address of "
                               "automatic variable %0; the pointer becomes "
                               "dangling when the function returns")
                        << VD << Ret->getSourceRange();
                    Check.diag(VD->getLocation(),
                               "automatic variable declared here",
                               DiagnosticIDs::Note);
                }

                void diagnoseReferenceReturn(ClangTidyCheck& Check,
                                             const ReturnStmt* Ret,
                                             const VarDecl* VD) {
                    Check.diag(Ret->getReturnLoc(),
                               "return statement returns a reference to "
                               "automatic variable %0; the reference becomes "
                               "dangling when the function returns")
                        << VD << Ret->getSourceRange();
                    Check.diag(VD->getLocation(),
                               "automatic variable declared here",
                               DiagnosticIDs::Note);
                }

                bool initStoresAddressOfAutomatic(const Expr* Init,
                                                  const VarDecl*& VD) {
                    VD = findAddressedAutomaticObject(Init);
                    return VD != nullptr;
                }

                void analyzeReturnedLambda(ClangTidyCheck& Check,
                                           const ReturnStmt* Ret,
                                           const LambdaExpr* LE) {
                    auto InitIt = LE->capture_init_begin();
                    for (const LambdaCapture& Cap : LE->captures()) {
                        const VarDecl* Captured = nullptr;
                        if (Cap.capturesVariable()) {
                            Captured = dyn_cast<VarDecl>(Cap.getCapturedVar());
                        }

                        if (Captured && isAutomaticObject(Captured) &&
                            Cap.getCaptureKind() == LCK_ByRef) {
                            Check.diag(Cap.getLocation(),
                                       "returned lambda captures automatic "
                                       "variable %0 by reference; the capture "
                                       "becomes dangling when the function "
                                       "returns")
                                << Captured << Ret->getSourceRange();
                            Check.diag(Captured->getLocation(),
                                       "automatic variable declared here",
                                       DiagnosticIDs::Note);
                        }

                        if (InitIt != LE->capture_init_end()) {
                            const VarDecl* Addressed = nullptr;
                            if (initStoresAddressOfAutomatic(*InitIt, Addressed)) {
                                Check.diag((*InitIt)->getExprLoc(),
                                           "returned lambda captures the address "
                                           "of automatic variable %0; the stored "
                                           "pointer becomes dangling when the "
                                           "function returns")
                                    << Addressed << Ret->getSourceRange();
                                Check.diag(Addressed->getLocation(),
                                           "automatic variable declared here",
                                           DiagnosticIDs::Note);
                            }
                            ++InitIt;
                        }
                    }
                }

                void findReturnedLambdas(ClangTidyCheck& Check,
                                         const ReturnStmt* Ret,
                                         const Stmt* S) {
                    if (!S) return;
                    if (const auto* LE = dyn_cast<LambdaExpr>(S)) {
                        analyzeReturnedLambda(Check, Ret, LE);
                        // Do not recurse into the lambda body: only the lambda
                        // object being returned matters here, not lambdas that
                        // may be nested in its body.
                        return;
                    }

                    for (const Stmt* Child : S->children()) {
                        findReturnedLambdas(Check, Ret, Child);
                    }
                }

            } // namespace

            SdcNoReturnStackAddressCheck::SdcNoReturnStackAddressCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoReturnStackAddressCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    returnStmt(
                        unless(isExpansionInSystemHeader()),
                        unless(isInTemplateInstantiation()),
                        hasAncestor(functionDecl().bind("function")),
                        hasReturnValue(expr().bind("ret")))
                        .bind("return"),
                    this);
            }

            void SdcNoReturnStackAddressCheck::check(
                const MatchFinder::MatchResult& Result) {
                const auto* Ret = Result.Nodes.getNodeAs<ReturnStmt>("return");
                const auto* E = Result.Nodes.getNodeAs<Expr>("ret");
                if (!Ret || !E) return;

                const auto* FD = Result.Nodes.getNodeAs<FunctionDecl>("function");
                QualType FuncRetTy = FD ? FD->getReturnType() : QualType{};

                if (!FuncRetTy.isNull() && FuncRetTy->isPointerType()) {
                    if (const VarDecl* VD = findAddressedAutomaticObject(E)) {
                        diagnoseAddressReturn(*this, Ret, VD);
                        // Continue below: the same return can also construct a
                        // wrapper from a bad lambda, and both facts are useful.
                    }
                }

                // Reference return: returning `x` or `x.member` where the base
                // object has automatic storage duration.  Restrict this to
                // functions that actually return by reference so that return
                // statements inside a returned lambda body are not mistaken for
                // an outer dangling-reference return.
                if (!FuncRetTy.isNull() && FuncRetTy->isReferenceType()) {
                    if (const VarDecl* VD = findBaseAutomaticObject(E)) {
                        diagnoseReferenceReturn(*this, Ret, VD);
                    }
                }

                findReturnedLambdas(*this, Ret, E);
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
