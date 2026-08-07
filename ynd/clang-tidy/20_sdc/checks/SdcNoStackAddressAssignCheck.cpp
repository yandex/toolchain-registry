#include "SdcNoStackAddressAssignCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                const VarDecl* findAddressedVar(
                    const Expr* E, SourceLocation Before, ASTContext& Ctx,
                    llvm::SmallPtrSetImpl<const VarDecl*>& Visiting);
                const VarDecl* storageBase(
                    const Expr* E, SourceLocation Before, ASTContext& Ctx,
                    llvm::SmallPtrSetImpl<const VarDecl*>& Visiting);

                bool before(SourceLocation A, SourceLocation B,
                            const SourceManager& SM) {
                    if (A.isInvalid() || B.isInvalid()) return false;
                    return SM.isBeforeInTranslationUnit(SM.getExpansionLoc(A),
                                                        SM.getExpansionLoc(B));
                }

                const FunctionDecl* enclosingFunction(const VarDecl* VD) {
                    const DeclContext* DC = VD ? VD->getDeclContext() : nullptr;
                    while (DC && !isa<FunctionDecl>(DC)) DC = DC->getParent();
                    return dyn_cast_or_null<FunctionDecl>(DC);
                }

                class LatestAssignmentVisitor
                    : public RecursiveASTVisitor<LatestAssignmentVisitor> {
                public:
                    LatestAssignmentVisitor(const VarDecl* Target,
                                            SourceLocation Before,
                                            const SourceManager& SM)
                        : Target(Target), Before(Before), SM(SM) {}

                    bool VisitBinaryOperator(BinaryOperator* BO) {
                        if (!BO->isAssignmentOp() ||
                            BO->getOpcode() != BO_Assign ||
                            !before(BO->getOperatorLoc(), Before, SM)) {
                            return true;
                        }
                        const Expr* LHS = BO->getLHS()->IgnoreParenImpCasts();
                        const auto* DRE = dyn_cast<DeclRefExpr>(LHS);
                        if (!DRE || DRE->getDecl() != Target) return true;
                        if (!Latest || before(Latest->getOperatorLoc(),
                                              BO->getOperatorLoc(), SM)) {
                            Latest = BO;
                        }
                        return true;
                    }

                    const BinaryOperator* get() const { return Latest; }

                private:
                    const VarDecl* Target;
                    SourceLocation Before;
                    const SourceManager& SM;
                    const BinaryOperator* Latest = nullptr;
                };

                // Resolve a pointer/reference parameter through direct calls in
                // this translation unit. This deliberately asks only whether
                // *some* call supplies a proven automatic object: that is
                // sufficient for a Required rule diagnostic at the assignment.
                class ParameterCallVisitor
                    : public RecursiveASTVisitor<ParameterCallVisitor> {
                public:
                    ParameterCallVisitor(const ParmVarDecl* Param,
                                         ASTContext& Ctx,
                                         llvm::SmallPtrSetImpl<const VarDecl*>& Visiting)
                        : Param(Param), Ctx(Ctx), Visiting(Visiting) {}

                    bool VisitCallExpr(CallExpr* CE) {
                        const auto* Owner = dyn_cast<FunctionDecl>(
                            Param->getDeclContext());
                        const FunctionDecl* Callee = CE->getDirectCallee();
                        if (!Owner || !Callee ||
                            Owner->getCanonicalDecl() !=
                                Callee->getCanonicalDecl()) {
                            return true;
                        }
                        unsigned Index = Param->getFunctionScopeIndex();
                        if (Index >= CE->getNumArgs()) return true;
                        if (const VarDecl* VD = findAddressedVar(
                                CE->getArg(Index), CE->getExprLoc(), Ctx,
                                Visiting)) {
                            Result = VD;
                            return false;
                        }
                        return true;
                    }

                    const VarDecl* get() const { return Result; }

                private:
                    const ParmVarDecl* Param;
                    ASTContext& Ctx;
                    llvm::SmallPtrSetImpl<const VarDecl*>& Visiting;
                    const VarDecl* Result = nullptr;
                };

                const VarDecl* resolveVar(const VarDecl* VD,
                                          SourceLocation Before,
                                          ASTContext& Ctx,
                                          llvm::SmallPtrSetImpl<const VarDecl*>& Visiting) {
                    if (!VD || !Visiting.insert(VD).second) return nullptr;

                    const VarDecl* Result = nullptr;
                    if (const auto* Param = dyn_cast<ParmVarDecl>(VD)) {
                        ParameterCallVisitor V(Param, Ctx, Visiting);
                        V.TraverseDecl(Ctx.getTranslationUnitDecl());
                        Result = V.get();
                    } else {
                        const FunctionDecl* FD = enclosingFunction(VD);
                        const BinaryOperator* Assignment = nullptr;
                        if (FD && FD->doesThisDeclarationHaveABody()) {
                            LatestAssignmentVisitor V(VD, Before,
                                                      Ctx.getSourceManager());
                            V.TraverseStmt(FD->getBody());
                            Assignment = V.get();
                        }
                        if (Assignment) {
                            Result = findAddressedVar(
                                Assignment->getRHS(),
                                Assignment->getOperatorLoc(), Ctx, Visiting);
                        } else if (VD->hasInit()) {
                            if (VD->getType()->isReferenceType()) {
                                Result = storageBase(VD->getInit(),
                                                     VD->getLocation(), Ctx,
                                                     Visiting);
                            } else {
                                Result = findAddressedVar(VD->getInit(),
                                                         VD->getLocation(), Ctx,
                                                         Visiting);
                            }
                        }
                    }
                    Visiting.erase(VD);
                    return Result;
                }

                // Find the automatic object whose storage is designated by an
                // lvalue. Member and array subobjects inherit the storage
                // duration of their complete object.
                const VarDecl* storageBase(const Expr* E,
                                           SourceLocation Before,
                                           ASTContext& Ctx,
                                           llvm::SmallPtrSetImpl<const VarDecl*>& Visiting) {
                    if (!E) return nullptr;
                    E = E->IgnoreParenImpCasts();
                    if (const auto* DRE = dyn_cast<DeclRefExpr>(E)) {
                        const auto* VD = dyn_cast<VarDecl>(DRE->getDecl());
                        if (!VD) return nullptr;
                        if (VD->getType()->isReferenceType()) {
                            return resolveVar(VD, Before, Ctx, Visiting);
                        }
                        return VD;
                    }
                    if (const auto* ME = dyn_cast<MemberExpr>(E)) {
                        if (ME->isArrow()) {
                            return findAddressedVar(ME->getBase(), Before, Ctx,
                                                    Visiting);
                        }
                        return storageBase(ME->getBase(), Before, Ctx, Visiting);
                    }
                    if (const auto* ASE = dyn_cast<ArraySubscriptExpr>(E)) {
                        return storageBase(ASE->getBase(), Before, Ctx, Visiting);
                    }
                    if (const auto* UO = dyn_cast<UnaryOperator>(E)) {
                        if (UO->getOpcode() == UO_Deref) {
                            return findAddressedVar(UO->getSubExpr(), Before,
                                                    Ctx, Visiting);
                        }
                    }
                    return nullptr;
                }

                const VarDecl* findAddressedVar(
                    const Expr* E, SourceLocation Before, ASTContext& Ctx,
                    llvm::SmallPtrSetImpl<const VarDecl*>& Visiting) {
                    if (!E) return nullptr;
                    E = E->IgnoreParens();

                    if (const auto* ICE = dyn_cast<ImplicitCastExpr>(E)) {
                        return findAddressedVar(ICE->getSubExpr(), Before, Ctx,
                                                Visiting);
                    }
                    if (const auto* UO = dyn_cast<UnaryOperator>(E)) {
                        if (UO->getOpcode() == UO_AddrOf) {
                            return storageBase(UO->getSubExpr(), Before, Ctx,
                                               Visiting);
                        }
                    }
                    if (const auto* BO = dyn_cast<BinaryOperator>(E)) {
                        if (BO->getOpcode() == BO_Add ||
                            BO->getOpcode() == BO_Sub) {
                            if (const VarDecl* VD = findAddressedVar(
                                    BO->getLHS(), Before, Ctx, Visiting)) {
                                return VD;
                            }
                            return findAddressedVar(BO->getRHS(), Before, Ctx,
                                                    Visiting);
                        }
                    }
                    if (const auto* DRE = dyn_cast<DeclRefExpr>(
                            E->IgnoreParenImpCasts())) {
                        const auto* VD = dyn_cast<VarDecl>(DRE->getDecl());
                        if (!VD) return nullptr;
                        if (VD->getType()->isArrayType()) return VD;
                        if (VD->getType()->isPointerType() ||
                            VD->getType()->isReferenceType()) {
                            return resolveVar(VD, Before, Ctx, Visiting);
                        }
                    }
                    if (isa<MemberExpr>(E->IgnoreParenImpCasts()) ||
                        isa<ArraySubscriptExpr>(E->IgnoreParenImpCasts())) {
                        return storageBase(E, Before, Ctx, Visiting);
                    }

                    // std::addressof(x)
                    if (const auto* CE = dyn_cast<CallExpr>(E)) {
                        const FunctionDecl* FD = CE->getDirectCallee();
                        if (FD && FD->getIdentifier() &&
                            FD->getName() == "addressof") {
                            const DeclContext* DC = FD->getDeclContext();
                            while (DC && DC->isInlineNamespace())
                                DC = DC->getParent();
                            if (const auto* NS = dyn_cast_or_null<NamespaceDecl>(DC)) {
                                if (NS->getIdentifier() &&
                                    NS->getName() == "std" &&
                                    NS->getParent() &&
                                    NS->getParent()->isTranslationUnit() &&
                                    CE->getNumArgs() >= 1) {
                                    return storageBase(CE->getArg(0), Before,
                                                       Ctx, Visiting);
                                }
                            }
                        }
                    }
                    return nullptr;
                }

                // A subscript through a pointer (as opposed to an embedded
                // array) or an arrow/dereference writes through storage whose
                // lifetime is not bounded by the local base object.
                bool targetHasPointerIndirection(const Expr* E) {
                    if (!E) return false;
                    E = E->IgnoreParenImpCasts();
                    if (const auto* ME = dyn_cast<MemberExpr>(E)) {
                        return ME->isArrow() ||
                               targetHasPointerIndirection(ME->getBase());
                    }
                    if (const auto* ASE = dyn_cast<ArraySubscriptExpr>(E)) {
                        const Expr* Base =
                            ASE->getBase()->IgnoreParenImpCasts();
                        if (!Base->getType()->isArrayType()) return true;
                        return targetHasPointerIndirection(Base);
                    }
                    if (const auto* UO = dyn_cast<UnaryOperator>(E)) {
                        return UO->getOpcode() == UO_Deref;
                    }
                    return false;
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

                llvm::SmallPtrSet<const VarDecl*, 16> Visiting;
                const VarDecl* RHSVar = findAddressedVar(
                    BO->getRHS(), BO->getOperatorLoc(), Ctx, Visiting);
                if (!RHSVar) return;
                if (classify(RHSVar) != LifetimeTier::Automatic) {
                    // RHS is &something_static / &something_global - rule
                    // does not apply.
                    return;
                }

                const bool IndirectTarget =
                    targetHasPointerIndirection(BO->getLHS());
                const VarDecl* LHSVar = findTargetBaseVar(BO->getLHS());
                if (!LHSVar) {
                    // E.g. `this->m = &local` - the complete object's
                    // lifetime is not available at this assignment site.
                    return;
                }

                LifetimeTier LT = classify(LHSVar);
                bool LongerLived = false;

                if (IndirectTarget || LT == LifetimeTier::StaticStorage) {
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
