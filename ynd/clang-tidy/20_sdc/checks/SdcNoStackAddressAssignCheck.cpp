#include "SdcNoStackAddressAssignCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
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

                // Walk through implicit casts, parens, and array-to-pointer
                // decay to find the leaf expression.
                const Expr* peelCasts(const Expr* E) {
                    if (!E) return nullptr;
                    return E->IgnoreParenImpCasts();
                }

                // From the RHS of an assignment, identify a VarDecl whose
                // address is being taken (directly via `&x`, via array decay
                // of `x`, or via a call to std::addressof). Returns nullptr if
                // the RHS does not match any of these forms.
                // Resolve a DeclRefExpr to the VarDecl whose storage the
                // address designates. Crucially, taking the address of a
                // *reference* variable yields the address of its referent, not
                // of the reference's own slot; the referent's storage duration
                // is not determinable from this point (it may be a container
                // element, a returned reference, a heap object, etc.), so such
                // cases are not stack-address escapes we can prove. Return
                // nullptr for reference-typed variables.
                const VarDecl* addressedVarFromDeclRef(const Expr* Sub) {
                    const auto* DRE = dyn_cast_or_null<DeclRefExpr>(Sub);
                    if (!DRE) return nullptr;
                    const auto* VD = dyn_cast<VarDecl>(DRE->getDecl());
                    if (!VD) return nullptr;
                    if (VD->getType()->isReferenceType()) return nullptr;
                    return VD;
                }

                const VarDecl* findAddressedVar(const Expr* RHS) {
                    const Expr* E = RHS ? RHS->IgnoreParens() : nullptr;
                    if (!E) return nullptr;

                    // `&x`
                    if (const auto* UO = dyn_cast<UnaryOperator>(E)) {
                        if (UO->getOpcode() == UO_AddrOf) {
                            const Expr* Sub = peelCasts(UO->getSubExpr());
                            return addressedVarFromDeclRef(Sub);
                        }
                        return nullptr;
                    }

                    // Array-to-pointer decay: an ImplicitCastExpr with kind
                    // CK_ArrayToPointerDecay wrapping a DeclRefExpr.
                    if (const auto* ICE = dyn_cast<ImplicitCastExpr>(RHS)) {
                        if (ICE->getCastKind() == CK_ArrayToPointerDecay) {
                            const Expr* Sub = ICE->getSubExpr()->IgnoreParens();
                            if (const auto* DRE = dyn_cast<DeclRefExpr>(Sub)) {
                                if (const auto* VD =
                                        dyn_cast<VarDecl>(DRE->getDecl())) {
                                    if (VD->getType()->isArrayType()) {
                                        return VD;
                                    }
                                }
                            }
                        }
                    }

                    // std::addressof(x)
                    if (const auto* CE = dyn_cast<CallExpr>(E)) {
                        const FunctionDecl* FD = CE->getDirectCallee();
                        if (FD && FD->getIdentifier() &&
                            FD->getName() == "addressof") {
                            // Be defensive: require the function to live in
                            // namespace std (or one of its inline namespaces).
                            const DeclContext* DC = FD->getDeclContext();
                            while (DC && DC->isInlineNamespace()) {
                                DC = DC->getParent();
                            }
                            if (DC && DC->isNamespace()) {
                                const auto* NS = cast<NamespaceDecl>(DC);
                                if (NS->getIdentifier() &&
                                    NS->getName() == "std" &&
                                    NS->getParent() &&
                                    NS->getParent()->isTranslationUnit()) {
                                    if (CE->getNumArgs() >= 1) {
                                        const Expr* Arg =
                                            peelCasts(CE->getArg(0));
                                        return addressedVarFromDeclRef(Arg);
                                    }
                                }
                            }
                        }
                    }

                    return nullptr;
                }

                // From the LHS of an assignment, identify the VarDecl whose
                // lifetime governs the assignment target:
                //  * a direct DeclRefExpr to a VarDecl  -> that VarDecl;
                //  * a MemberExpr `obj.m` (or `obj->m`) -> the base VarDecl
                //    found recursively;
                //  * `this->m` -> nullptr (unknown base lifetime, skip).
                const VarDecl* findTargetBaseVar(const Expr* LHS) {
                    const Expr* E = LHS ? LHS->IgnoreParenImpCasts() : nullptr;
                    while (E) {
                        if (const auto* DRE = dyn_cast<DeclRefExpr>(E)) {
                            return dyn_cast<VarDecl>(DRE->getDecl());
                        }
                        if (const auto* ME = dyn_cast<MemberExpr>(E)) {
                            const Expr* Base = ME->getBase();
                            if (!Base) return nullptr;
                            Base = Base->IgnoreParenImpCasts();
                            // `this->m` or otherwise opaque base.
                            if (isa<CXXThisExpr>(Base)) return nullptr;
                            E = Base;
                            continue;
                        }
                        if (const auto* ASE = dyn_cast<ArraySubscriptExpr>(E)) {
                            E = ASE->getBase()->IgnoreParenImpCasts();
                            continue;
                        }
                        return nullptr;
                    }
                    return nullptr;
                }

                // The nearest enclosing CompoundStmt of the given VarDecl, or
                // the function body's CompoundStmt if the VarDecl is a
                // parameter. Returns nullptr if the VarDecl has static
                // storage or is not within a function.
                const CompoundStmt* enclosingBlock(const VarDecl* VD,
                                                   ASTContext& Ctx) {
                    if (!VD) return nullptr;
                    if (VD->hasGlobalStorage() && !VD->isStaticLocal()) {
                        return nullptr;
                    }
                    if (VD->isStaticLocal()) {
                        return nullptr;
                    }
                    if (const auto* PV = dyn_cast<ParmVarDecl>(VD)) {
                        const auto* FD = dyn_cast_or_null<FunctionDecl>(
                            PV->getDeclContext());
                        if (FD && FD->doesThisDeclarationHaveABody()) {
                            return dyn_cast_or_null<CompoundStmt>(FD->getBody());
                        }
                        return nullptr;
                    }
                    DynTypedNode N = DynTypedNode::create(*VD);
                    for (int Depth = 0; Depth < 64; ++Depth) {
                        auto Parents = Ctx.getParents(N);
                        if (Parents.empty()) return nullptr;
                        N = Parents[0];
                        if (const auto* CS = N.get<CompoundStmt>()) {
                            return CS;
                        }
                        if (N.get<FunctionDecl>() || N.get<TranslationUnitDecl>()) {
                            return nullptr;
                        }
                    }
                    return nullptr;
                }

                // True if Outer is a (strict or non-strict) ancestor of Inner
                // in the AST.
                bool isAncestorOf(const CompoundStmt* Outer,
                                  const CompoundStmt* Inner,
                                  ASTContext& Ctx) {
                    if (!Outer || !Inner) return false;
                    if (Outer == Inner) return true;
                    DynTypedNode N = DynTypedNode::create(*Inner);
                    for (int Depth = 0; Depth < 64; ++Depth) {
                        auto Parents = Ctx.getParents(N);
                        if (Parents.empty()) return false;
                        N = Parents[0];
                        if (const auto* CS = N.get<CompoundStmt>()) {
                            if (CS == Outer) return true;
                        }
                        if (N.get<TranslationUnitDecl>()) return false;
                    }
                    return false;
                }

                // Classify a VarDecl's lifetime relative to function scope.
                enum class LifetimeTier {
                    StaticStorage,  // namespace-scope, static local, static member, thread_local
                    Automatic,
                    Unknown
                };

                LifetimeTier classify(const VarDecl* VD) {
                    if (!VD) return LifetimeTier::Unknown;
                    if (VD->hasGlobalStorage()) return LifetimeTier::StaticStorage;
                    // ParmVarDecl and non-static locals are automatic.
                    return LifetimeTier::Automatic;
                }

            } // namespace

            SdcNoStackAddressAssignCheck::SdcNoStackAddressAssignCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoStackAddressAssignCheck::registerMatchers(
                MatchFinder* Finder) {
                // Built-in `=` between pointer operands. We do the structural
                // inspection in check() so we can share helpers with future
                // overloaded-operator support.
                Finder->addMatcher(
                    binaryOperator(
                        unless(isExpansionInSystemHeader()),
                        hasOperatorName("="))
                        .bind("assign"),
                    this);
            }

            void SdcNoStackAddressAssignCheck::check(
                const MatchFinder::MatchResult& Result) {
                const auto* BO = Result.Nodes.getNodeAs<BinaryOperator>("assign");
                if (!BO) return;

                ASTContext& Ctx = *Result.Context;

                const VarDecl* RHSVar = findAddressedVar(BO->getRHS());
                if (!RHSVar) return;
                if (classify(RHSVar) != LifetimeTier::Automatic) {
                    // RHS is &something_static / &something_global - rule
                    // does not apply.
                    return;
                }

                const VarDecl* LHSVar = findTargetBaseVar(BO->getLHS());
                if (!LHSVar) {
                    // E.g. `this->m = &local` - opaque, do not flag here.
                    return;
                }

                LifetimeTier LT = classify(LHSVar);
                bool LongerLived = false;

                if (LT == LifetimeTier::StaticStorage) {
                    LongerLived = true;
                } else if (LT == LifetimeTier::Automatic) {
                    const CompoundStmt* LBlk = enclosingBlock(LHSVar, Ctx);
                    const CompoundStmt* RBlk = enclosingBlock(RHSVar, Ctx);
                    if (LBlk && RBlk && LBlk != RBlk &&
                        isAncestorOf(LBlk, RBlk, Ctx)) {
                        LongerLived = true;
                    }
                }

                if (!LongerLived) return;

                diag(BO->getOperatorLoc(),
                     "assigning the address of automatic variable %0 to %1, "
                     "which has a longer lifetime")
                    << RHSVar << LHSVar
                    << BO->getSourceRange();
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
