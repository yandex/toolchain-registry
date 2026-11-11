#include "SdcMovedFromStateCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprConcepts.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/Analysis/Analyses/ExprMutationAnalyzer.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <optional>

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcMovedFromStateCheck::SdcMovedFromStateCheck(StringRef Name,
                                                ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

// ─── Helpers ────────────────────────────────────────────────────────────────

static bool isStdMoveOrForward(const CallExpr* CE) {
    const Expr* Callee = CE->getCallee()->IgnoreParenCasts();

    auto nameIs = [](const NamedDecl* D, StringRef N) {
        return D && D->getIdentifier() && D->getName() == N;
    };
    auto inStd = [](const NamedDecl* D) -> bool {
        if (!D) return false;
        const auto* NS = dyn_cast<NamespaceDecl>(D->getDeclContext());
        return NS && NS->isStdNamespace();
    };

    if (const auto* DRE = dyn_cast<DeclRefExpr>(Callee)) {
        const NamedDecl* D = DRE->getDecl();
        return (nameIs(D, "move") || nameIs(D, "forward")) && inStd(D);
    }
    if (const auto* ULE = dyn_cast<UnresolvedLookupExpr>(Callee)) {
        const std::string N = ULE->getName().getAsString();
        if (N != "move" && N != "forward") return false;
        for (const NamedDecl* D : ULE->decls())
            if (inStd(D)) return true;
    }
    return false;
}

// Returns true if E is a static_cast<T&&>(expr) — equivalent to move.
// NOTE: SC->getType() strips the reference (returns the value type with
// xvalue category). We must check getTypeAsWritten() which preserves the
// written T&& type.
static bool isRValueCast(const Expr* E) {
    const auto* SC = dyn_cast<CXXStaticCastExpr>(E);
    if (!SC) return false;
    return SC->getTypeAsWritten()->isRValueReferenceType();
}

// Returns the VarDecl being moved/forwarded, or nullptr.
static const VarDecl* getMovedVarDecl(const Expr* MoveExpr) {
    const Expr* Arg = nullptr;

    if (const auto* CE = dyn_cast<CallExpr>(MoveExpr)) {
        if (CE->getNumArgs() == 0) return nullptr;
        Arg = CE->getArg(0)->IgnoreParenImpCasts();
    } else if (const auto* SC = dyn_cast<CXXStaticCastExpr>(MoveExpr)) {
        Arg = SC->getSubExpr()->IgnoreParenImpCasts();
    }

    if (!Arg) return nullptr;
    const auto* DRE = dyn_cast<DeclRefExpr>(Arg);
    if (!DRE) return nullptr;
    return dyn_cast<VarDecl>(DRE->getDecl());
}

static const VarDecl* getDirectVarDecl(const Expr* E) {
    if (!E) return nullptr;
    const auto* DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
    return DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
}

static const CallExpr* stdTie(const Expr* E) {
    if (!E) return nullptr;
    const auto* Call = dyn_cast<CallExpr>(E->IgnoreParenImpCasts()->IgnoreImplicit());
    const auto* FD = Call ? Call->getDirectCallee() : nullptr;
    if (!FD || !FD->getIdentifier() || FD->getName() != "tie") return nullptr;
    const auto* NS = dyn_cast<NamespaceDecl>(FD->getDeclContext());
    while (NS && NS->isInline()) NS = dyn_cast<NamespaceDecl>(NS->getParent());
    return NS && NS->isStdNamespace() ? Call : nullptr;
}

// std::move/std::forward and an rvalue cast only change value category.  When
// the result initializes a reference, no move operation consumes the object.
static bool onlyBindsReference(const Expr* E, ASTContext& Ctx) {
    const Expr* Current = E;
    while (Current) {
        const auto Parents = Ctx.getParents(*Current);
        if (Parents.size() != 1) return false;

        if (const auto* VD = Parents[0].get<VarDecl>())
            return VD->getType()->isReferenceType();

        const auto* Parent = Parents[0].get<Expr>();
        if (!Parent) return false;
        if (!isa<ParenExpr>(Parent) && !isa<ImplicitCastExpr>(Parent) &&
            !isa<ExprWithCleanups>(Parent) &&
            !isa<MaterializeTemporaryExpr>(Parent) &&
            !isa<CXXBindTemporaryExpr>(Parent))
            return false;
        Current = Parent;
    }
    return false;
}

static bool isStdClearCall(const CXXMemberCallExpr* MCE) {
    const auto* MD = MCE->getMethodDecl();
    if (!MD || !MD->getIdentifier() || MD->getName() != "clear" ||
        MD->getNumParams() != 0)
        return false;

    for (const DeclContext* DC = MD->getDeclContext(); DC;
         DC = DC->getParent())
        if (const auto* NS = dyn_cast<NamespaceDecl>(DC))
            if (NS->isStdNamespace()) return true;
    return false;
}

// Standard smart pointers have a specified empty/null state after move and
// may be queried or moved again safely.
static bool hasSpecifiedMovedFromState(const VarDecl* VD) {
    QualType T = VD->getType().getCanonicalType();
    const auto* RD = T->getAsCXXRecordDecl();
    if (!RD) return false;
    if (!RD->getIdentifier() ||
        (RD->getName() != "unique_ptr" && RD->getName() != "shared_ptr"))
        return false;
    const auto* NS = dyn_cast<NamespaceDecl>(RD->getDeclContext());
    return NS && NS->isStdNamespace();
}

// Only source-level named objects can subsequently be referred to by user
// code.  Compiler-generated and unnamed parameters otherwise produce blank,
// unactionable diagnostics at their enclosing declaration.
static bool isTrackableMovedObject(const VarDecl* VD) {
    return VD && !VD->isImplicit() && VD->getIdentifier();
}

// ─── Per-function visitor ────────────────────────────────────────────────────

class MovedFromVisitor : public RecursiveASTVisitor<MovedFromVisitor> {
    ASTContext& Ctx;
    ClangTidyCheck& Check;

    // VD → source location of the move that put it in the moved-from state.
    struct MoveInfo {
        SourceLocation Location;
        llvm::DenseMap<const VarDecl*, bool> Booleans;
        const ForStmt* LastIteration = nullptr;
    };
    using MoveState = llvm::DenseMap<const VarDecl*, MoveInfo>;
    const ForStmt* ActiveLastIteration = nullptr;
    llvm::SmallVector<const ForStmt*, 4> ForLoops;

    void recordMove(const VarDecl* VD, SourceLocation Location) {
        MoveInfo Info;
        Info.Location = Location;
        Info.LastIteration = ActiveLastIteration;
        MovedFrom[VD] = std::move(Info);
    }

    // Facts are attached to moved paths, rather than the merged execution
    // state. E.g. moving x and then setting processed=true makes !processed
    // incompatible with that move even when an unmoved path also reaches it.
    void filterBoolean(const Expr* E, bool Value) {
        E = E->IgnoreParenImpCasts();
        if (const auto* U = dyn_cast<UnaryOperator>(E)) {
            if (U->getOpcode() == UO_LNot) { filterBoolean(U->getSubExpr(), !Value); return; }
        }
        const auto* VD = getDirectVarDecl(E);
        if (!VD || !VD->getType()->isBooleanType()) return;
        llvm::SmallVector<const VarDecl*, 4> Remove;
        for (const auto& [Object, Info] : MovedFrom) {
            auto I = Info.Booleans.find(VD);
            if (I != Info.Booleans.end() && I->second != Value) Remove.push_back(Object);
        }
        for (const VarDecl* Object : Remove) MovedFrom.erase(Object);
    }

    llvm::DenseSet<const VarDecl*> EscapingFlags;

    void assignBoolean(const VarDecl* VD, const Expr* RHS) {
        if (!VD || !VD->isLocalVarDecl() || VD->getType().isVolatileQualified() ||
            EscapingFlags.contains(VD) || !VD->getType()->isBooleanType()) return;
        auto Value = RHS ? RHS->getIntegerConstantExpr(Ctx) : std::nullopt;
        for (auto& [Object, Info] : MovedFrom) {
            if (Value) Info.Booleans[VD] = *Value != 0;
            else Info.Booleans.erase(VD);
        }
    }

    const ForStmt* lastIterationGuard(const Expr* E) {
        if (ForLoops.empty()) return nullptr;
        const ForStmt* Loop = ForLoops.back();
        const auto* Cond = dyn_cast_or_null<BinaryOperator>(Loop->getCond());
        const auto* Inc = dyn_cast_or_null<UnaryOperator>(Loop->getInc());
        const auto* Eq = dyn_cast<BinaryOperator>(E->IgnoreParenImpCasts());
        if (!Cond || Cond->getOpcode() != BO_LT || !Inc || !Inc->isIncrementOp() ||
            !Eq || Eq->getOpcode() != BO_EQ) return nullptr;
        const VarDecl* Index = getDirectVarDecl(Cond->getLHS());
        if (!Index || !Index->getType()->isUnsignedIntegerType() ||
            getDirectVarDecl(Inc->getSubExpr()) != Index) return nullptr;
        const auto* Add = dyn_cast<BinaryOperator>(Eq->getLHS()->IgnoreParenImpCasts());
        if (!Add || Add->getOpcode() != BO_Add || getDirectVarDecl(Add->getLHS()) != Index ||
            !Ctx.hasSameUnqualifiedType(Add->getType(), Index->getType()) ||
            !Ctx.hasSameUnqualifiedType(Cond->getRHS()->getType(), Index->getType())) return nullptr;
        auto One = Add->getRHS()->getIntegerConstantExpr(Ctx);
        if (!One || *One != 1) return nullptr;
        llvm::FoldingSetNodeID Bound, GuardBound;
        Cond->getRHS()->IgnoreParenImpCasts()->Profile(Bound, Ctx, true);
        Eq->getRHS()->IgnoreParenImpCasts()->Profile(GuardBound, Ctx, true);
        if (Bound != GuardBound) return nullptr;
        // Restrict the proof to a stable bound and an unmodified induction
        // variable. In particular, a mutable container's size is not stable.
        ExprMutationAnalyzer Mutations(*Loop->getBody(), Ctx);
        if (Mutations.isMutated(Index)) return nullptr;
        const Expr* Limit = Cond->getRHS()->IgnoreParenImpCasts();
        const VarDecl* BoundVar = getDirectVarDecl(Limit);
        if (const auto* Call = dyn_cast<CXXMemberCallExpr>(Limit)) {
            const auto* Method = Call->getMethodDecl();
            if (!Method || !Method->isConst() || Method->getName() != "size" || Call->getNumArgs()) return nullptr;
            const auto* Record = Method->getParent();
            if (!Record->getIdentifier() || Record->getName() != "vector" || !Record->isInStdNamespace()) return nullptr;
            BoundVar = getDirectVarDecl(Call->getImplicitObjectArgument());
            if (!BoundVar || !BoundVar->getType().getNonReferenceType().isConstQualified()) return nullptr;
        } else if (!BoundVar && !Limit->getIntegerConstantExpr(Ctx)) return nullptr;
        if (BoundVar && Mutations.isMutated(BoundVar)) return nullptr;
        return Loop;
    }
    MoveState MovedFrom;
    bool Reachable = true;

    struct FlowState {
        MoveState Moved = MoveState();
        bool Reachable = false;
    };
    struct ControlFrame {
        bool IsLoop;
        FlowState Breaks;
        FlowState Continues;
    };
    llvm::SmallVector<ControlFrame, 4> Controls;
    llvm::SmallVector<FlowState, 2> Exceptions;

    FlowState flow() const { return {MovedFrom, Reachable}; }
    void restore(const FlowState& F) {
        MovedFrom = F.Moved;
        Reachable = F.Reachable;
    }
    static void join(FlowState& Into, const FlowState& From) {
        if (!From.Reachable) return;
        if (!Into.Reachable) {
            Into = From;
            return;
        }
        Into.Reachable = true;
        for (const auto& [VD, Info] : From.Moved) {
            auto [I, Inserted] = Into.Moved.try_emplace(VD, Info);
            if (Inserted) continue;
            auto& Existing = I->second;
            if (Existing.LastIteration != Info.LastIteration) Existing.LastIteration = nullptr;
            llvm::SmallVector<const VarDecl*, 4> Forget;
            for (const auto& [Flag, Value] : Existing.Booleans) {
                auto F = Info.Booleans.find(Flag);
                if (F == Info.Booleans.end() || F->second != Value) Forget.push_back(Flag);
            }
            for (const VarDecl* Flag : Forget) Existing.Booleans.erase(Flag);
        }
    }

    // Loop re-traversal deliberately visits some AST nodes more than once.
    // Keep diagnostics unique by spelling location.
    llvm::DenseSet<unsigned> ReportedUses;

    // Guards against recursive re-entry while processing the argument of a move.
    bool InMoveArg = false;

public:
    MovedFromVisitor(ASTContext& C, ClangTidyCheck& Ch) : Ctx(C), Check(Ch) {}

    bool dataTraverseStmtPre(Stmt* S) {
        // Type queries and requirements do not execute their operands. Prune
        // before any use, move or assignment callback can change the state.
        // Clang retains evaluated operands such as polymorphic typeid.
        return Reachable && !isa<RequiresExpr>(S) &&
               !ExprMutationAnalyzer::isUnevaluated(S, Ctx);
    }

    void run(const FunctionDecl* FD) {
        struct FlagUses : RecursiveASTVisitor<FlagUses> {
            ASTContext& Ctx;
            llvm::DenseSet<const VarDecl*>& Escaping;
            FlagUses(ASTContext& C, llvm::DenseSet<const VarDecl*>& E) : Ctx(C), Escaping(E) {}
            bool VisitDeclRefExpr(DeclRefExpr* E) {
                const auto* VD = dyn_cast<VarDecl>(E->getDecl());
                if (!VD || !VD->getType()->isBooleanType()) return true;
                DynTypedNode N = DynTypedNode::create(*E);
                while (true) {
                    auto Parents = Ctx.getParents(N);
                    if (Parents.size() != 1) break;
                    const auto& P = Parents[0];
                    if (P.get<ParenExpr>()) { N = P; continue; }
                    if (const auto* Cast = P.get<ImplicitCastExpr>())
                        if (Cast->getCastKind() == CK_LValueToRValue) return true;
                    if (const auto* Assign = P.get<BinaryOperator>())
                        if (Assign->getOpcode() == BO_Assign && Assign->getLHS() == N.get<Expr>()) return true;
                    break;
                }
                Escaping.insert(VD);
                return true;
            }
        };
        if (const Stmt* Body = FD->getBody()) {
            FlagUses Uses(Ctx, EscapingFlags);
            Uses.TraverseStmt(const_cast<Stmt*>(Body));
            TraverseStmt(const_cast<Stmt*>(Body));
        }
    }

    // ── CallExpr: detect std::move/forward and use-of-moved-from ─────────────
    bool TraverseCallExpr(CallExpr* CE) {
        // std::tie only binds references. Argument subexpressions can still
        // execute, but naming a referent is neither a read nor a reset.
        if (stdTie(CE)) return RecursiveASTVisitor::TraverseCallExpr(CE);
        if (isStdMoveOrForward(CE)) {
            const VarDecl* VD = getMovedVarDecl(CE);
            if (!onlyBindsReference(CE, Ctx) &&
                isTrackableMovedObject(VD) &&
                !hasSpecifiedMovedFromState(VD)) {
                if (MovedFrom.count(VD) != 0 && CE->getNumArgs() != 0)
                    checkUse(CE->getArg(0));
                recordMove(VD, CE->getBeginLoc());
                // Do NOT recurse into the argument — we don't want to
                // flag the DeclRefExpr inside the move call as a "use".
                return true;
            }
        }

        // Check arguments for use of moved-from variables.
        if (!InMoveArg) {
            for (unsigned I = 0; I < CE->getNumArgs(); ++I)
                checkUse(CE->getArg(I));
        }

        return RecursiveASTVisitor::TraverseCallExpr(CE);
    }

    bool TraverseCXXStaticCastExpr(CXXStaticCastExpr* SC) {
        if (isRValueCast(SC)) {
            const VarDecl* VD = getMovedVarDecl(SC);
            if (!onlyBindsReference(SC, Ctx) &&
                isTrackableMovedObject(VD) &&
                !hasSpecifiedMovedFromState(VD)) {
                if (MovedFrom.count(VD) != 0)
                    checkUse(SC->getSubExpr());
                recordMove(VD, SC->getBeginLoc());
                return true; // Don't recurse into operand
            }
        }
        return RecursiveASTVisitor::TraverseCXXStaticCastExpr(SC);
    }

    // ── Member calls: check implicit object for moved-from state ─────────────
    bool TraverseCXXMemberCallExpr(CXXMemberCallExpr* MCE) {
        if (isStdClearCall(MCE))
            MovedFrom.erase(getDirectVarDecl(MCE->getImplicitObjectArgument()));
        else
            checkUse(MCE->getImplicitObjectArgument());
        return RecursiveASTVisitor::TraverseCXXMemberCallExpr(MCE);
    }

    // RecursiveASTVisitor dispatches overloaded operators through this
    // method rather than TraverseCallExpr.  Check operands explicitly so
    // stream insertion and other free operators count as uses.
    bool TraverseCXXOperatorCallExpr(CXXOperatorCallExpr* OC) {
        if (OC->getOperator() == OO_Equal && OC->getNumArgs() == 2) {
            if (const auto* Tied = stdTie(OC->getArg(0))) {
                checkUse(OC->getArg(1));
                if (const auto* Source = stdTie(OC->getArg(1)))
                    for (const Expr* Arg : Source->arguments()) checkUse(Arg);
                if (!TraverseStmt(OC->getArg(1)) ||
                    !TraverseStmt(OC->getArg(0))) return false;
                for (const Expr* Arg : Tied->arguments())
                    MovedFrom.erase(getDirectVarDecl(Arg));
                return true;
            }
            if (const auto* Destination = getDirectVarDecl(OC->getArg(0))) {
                // In assignment syntax the RHS is evaluated before the LHS.
                // Its reads see the old state; only completion restores the
                // destination, including x = transform(std::move(x)).
                checkUse(OC->getArg(1));
                if (!TraverseStmt(OC->getArg(1)) ||
                    !TraverseStmt(OC->getArg(0)) ||
                    !TraverseStmt(OC->getCallee())) return false;
                MovedFrom.erase(Destination);
                return true;
            }
        }
        if (OC->getOperator() != OO_Equal)
            for (const Expr* Arg : OC->arguments()) checkUse(Arg);
        return RecursiveASTVisitor::TraverseCXXOperatorCallExpr(OC);
    }

    // Sequence only paths which can still reach the next statement. Abrupt
    // exits retain their state in the destination frame, not in this sequence.
    bool TraverseCompoundStmt(CompoundStmt* S) {
        for (Stmt* Child : S->body()) {
            if (!Reachable) break;
            if (!TraverseStmt(Child)) return false;
        }
        return true;
    }

    bool TraverseReturnStmt(ReturnStmt* S) {
        if (!RecursiveASTVisitor::TraverseReturnStmt(S)) return false;
        Reachable = false;
        return true;
    }
    bool TraverseContinueStmt(ContinueStmt*) {
        for (auto I = Controls.rbegin(); I != Controls.rend(); ++I) {
            if (!I->IsLoop) continue;
            join(I->Continues, flow());
            Reachable = false;
            break;
        }
        return true;
    }
    bool TraverseBreakStmt(BreakStmt*) {
        if (!Controls.empty()) {
            join(Controls.back().Breaks, flow());
            Reachable = false;
        }
        return true;
    }
    bool TraverseCXXThrowExpr(CXXThrowExpr* S) {
        if (!RecursiveASTVisitor::TraverseCXXThrowExpr(S)) return false;
        if (!Exceptions.empty()) join(Exceptions.back(), flow());
        Reachable = false;
        return true;
    }
    bool TraverseCXXTryStmt(CXXTryStmt* S) {
        const auto Entry = flow();
        Exceptions.emplace_back();
        if (!TraverseStmt(S->getTryBlock())) return false;
        auto Normal = flow();
        auto Exceptional = Exceptions.pop_back_val();
        // Preserve the incoming state for unknown exceptional entries. Full
        // propagation of implicit exceptions from expressions remains unclaimed.
        join(Exceptional, Entry);
        for (unsigned I = 0; I < S->getNumHandlers(); ++I) {
            restore(Exceptional);
            if (!TraverseStmt(S->getHandler(I))) return false;
            join(Normal, flow());
        }
        restore(Normal);
        return true;
    }

    // Function bodies have their own matcher invocation. Their return/break
    // state must never change the enclosing function merely by declaring them.
    bool TraverseLambdaExpr(LambdaExpr* E) {
        for (Expr* Init : E->capture_inits())
            if (!TraverseStmt(Init)) return false;
        return true;
    }
    bool TraverseDecl(Decl* D) {
        if (D && isa<FunctionDecl>(D)) return true;
        return RecursiveASTVisitor::TraverseDecl(D);
    }

    bool TraverseIfStmt(IfStmt* S) {
        if (!TraverseStmt(S->getInit()) ||
            !TraverseStmt(S->getConditionVariableDeclStmt()) ||
            !TraverseStmt(S->getCond())) return false;
        if (S->isConstexpr()) {
            if (auto Selected = S->getNondiscardedCase(Ctx))
                return TraverseStmt(*Selected);
            return true;
        }
        const auto Entry = flow();
        const ForStmt* PreviousLast = ActiveLastIteration;
        if (const ForStmt* Last = lastIterationGuard(S->getCond())) ActiveLastIteration = Last;
        filterBoolean(S->getCond(), true);
        if (!TraverseStmt(S->getThen())) return false;
        ActiveLastIteration = PreviousLast;
        auto Exit = flow();
        restore(Entry);
        filterBoolean(S->getCond(), false);
        if (!TraverseStmt(S->getElse())) return false;
        join(Exit, flow());
        restore(Exit);
        return true;
    }

    bool TraverseConditionalOperator(ConditionalOperator* E) {
        if (!TraverseStmt(E->getCond())) return false;
        auto Branch = [&](Expr* Arm) {
            if (!onlyBindsReference(E, Ctx)) checkUse(Arm);
            return TraverseStmt(Arm);
        };
        if (auto Constant = E->getCond()->getIntegerConstantExpr(Ctx))
            return Branch(*Constant != 0 ? E->getTrueExpr() : E->getFalseExpr());
        const auto Entry = flow();
        if (!Branch(E->getTrueExpr())) return false;
        auto Exit = flow();
        restore(Entry);
        if (!Branch(E->getFalseExpr())) return false;
        join(Exit, flow());
        restore(Exit);
        return true;
    }

    bool TraverseSwitchStmt(SwitchStmt* S) {
        if (!TraverseStmt(S->getInit()) ||
            !TraverseStmt(S->getConditionVariableDeclStmt()) ||
            !TraverseStmt(S->getCond())) return false;
        const auto* Body = dyn_cast<CompoundStmt>(S->getBody());
        if (!Body) return true;

        struct Arm { llvm::SmallVector<Stmt*, 4> Statements; };
        llvm::SmallVector<Arm, 8> Arms;
        llvm::SmallPtrSet<const SwitchCase*, 16> Labels;
        bool HasDefault = false;
        for (Stmt* Child : Body->body()) {
            if (isa<SwitchCase>(Child)) {
                Arms.emplace_back();
                while (auto* Label = dyn_cast<SwitchCase>(Child)) {
                    Labels.insert(Label);
                    HasDefault |= isa<DefaultStmt>(Label);
                    Child = Label->getSubStmt();
                }
            }
            if (!Arms.empty()) Arms.back().Statements.push_back(Child);
        }
        // Labels nested in arbitrary blocks/loops need a CFG (e.g. Duff's
        // device). Do not invent sequential edges for unsupported dispatch.
        for (const SwitchCase* Label = S->getSwitchCaseList(); Label;
             Label = Label->getNextSwitchCase())
            if (!Labels.count(Label)) return true;

        const auto Entry = flow();
        Controls.push_back({false, {}, {}});
        FlowState Falling;
        for (const auto& Arm : Arms) {
            auto Incoming = Entry;
            join(Incoming, Falling); // Actual fallthrough from the previous arm.
            restore(Incoming);
            for (Stmt* Child : Arm.Statements) {
                if (!Reachable) break;
                if (!TraverseStmt(Child)) return false;
            }
            Falling = flow();
        }
        auto Exit = Controls.pop_back_val().Breaks;
        join(Exit, Falling);
        if (!HasDefault) join(Exit, Entry); // No matching case.
        restore(Exit);
        return true;
    }

    // Keep the existing two-iteration bound, but route continue through the
    // increment/condition and break directly to the exit. Range bindings and
    // body-local declarations are recreated each iteration.
    bool traverseLoop(Stmt* Body, Stmt* Condition, Stmt* Increment,
                      Stmt* ConditionVariable, Stmt* LoopVariable, bool IsDo,
                      const ForStmt* CurrentLoop = nullptr) {
        Controls.push_back({true, {}, {}});
        FlowState Exit;
        auto Test = [&]() {
            if (!TraverseStmt(ConditionVariable) || !TraverseStmt(Condition)) return false;
            const auto* E = dyn_cast_or_null<Expr>(Condition);
            auto Constant = E ? E->getIntegerConstantExpr(Ctx) : std::nullopt;
            if (Condition && (!Constant || *Constant == 0)) join(Exit, flow());
            if (Constant && *Constant == 0) Reachable = false;
            return true;
        };
        if (!IsDo && !Test()) return false;
        for (unsigned Iteration = 0; Iteration < 2 && Reachable; ++Iteration) {
            Controls.back().Continues = {};
            if (!TraverseStmt(LoopVariable) || !TraverseStmt(Body)) return false;
            auto Next = flow();
            join(Next, Controls.back().Continues);
            restore(Next);
            if (!Reachable) break;
            if (CurrentLoop) {
                // These moved paths reach the loop exit after incrementing
                // the last index. Retain them at exit, not on the back edge.
                FlowState Last = flow();
                Last.Moved.clear();
                llvm::SmallVector<const VarDecl*, 4> Remove;
                for (const auto& [VD, Info] : MovedFrom) {
                    if (Info.LastIteration == CurrentLoop) {
                        auto Exiting = Info;
                        Exiting.LastIteration = nullptr;
                        Last.Moved[VD] = Exiting;
                        Remove.push_back(VD);
                    }
                }
                if (!Remove.empty()) join(Exit, Last);
                for (const VarDecl* VD : Remove) MovedFrom.erase(VD);
            }
            if (!TraverseStmt(Increment) || !Test()) return false;
        }
        join(Exit, Controls.pop_back_val().Breaks);
        for (auto& [VD, Info] : Exit.Moved)
            if (Info.LastIteration == CurrentLoop) Info.LastIteration = nullptr;
        restore(Exit);
        return true;
    }
    bool TraverseForStmt(ForStmt* S) {
        if (!TraverseStmt(S->getInit())) return false;
        ForLoops.push_back(S);
        bool Result = traverseLoop(S->getBody(), S->getCond(), S->getInc(),
                                   S->getConditionVariableDeclStmt(), nullptr, false, S);
        ForLoops.pop_back();
        return Result;
    }
    bool TraverseCXXForRangeStmt(CXXForRangeStmt* S) {
        if (!TraverseStmt(S->getInit()) || !TraverseStmt(S->getRangeStmt()) ||
            !TraverseStmt(S->getBeginStmt()) || !TraverseStmt(S->getEndStmt())) return false;
        return traverseLoop(S->getBody(), S->getCond(), S->getInc(),
                            nullptr, S->getLoopVarStmt(), false);
    }
    bool TraverseWhileStmt(WhileStmt* S) {
        return traverseLoop(S->getBody(), S->getCond(), nullptr,
                            S->getConditionVariableDeclStmt(), nullptr, false);
    }
    bool TraverseDoStmt(DoStmt* S) {
        return traverseLoop(S->getBody(), S->getCond(), nullptr, nullptr, nullptr, true);
    }

    // Each execution of a declaration creates a fresh object.  This is
    // especially important during the bounded second traversal of loop
    // bodies: a variable declared inside the body is not moved-from merely
    // because the previous iteration's distinct object was moved.
    bool VisitVarDecl(VarDecl* VD) {
        MovedFrom.erase(VD);
        assignBoolean(VD, VD->getInit());
        return true;
    }

    bool TraverseBinaryOperator(BinaryOperator* BO) {
        if (BO->getOpcode() == BO_Assign) {
            if (const auto* Destination = getDirectVarDecl(BO->getLHS())) {
                checkUse(BO->getRHS());
                if (!TraverseStmt(BO->getRHS()) ||
                    !TraverseStmt(BO->getLHS())) return false;
                MovedFrom.erase(Destination);
                assignBoolean(Destination, BO->getRHS());
                return true;
            }
        }
        return RecursiveASTVisitor::TraverseBinaryOperator(BO);
    }

    // Compound assignments retain their existing treatment.
    bool VisitBinaryOperator(BinaryOperator* BO) {
        if (BO->isCompoundAssignmentOp()) {
            const auto* DRE = dyn_cast<DeclRefExpr>(
                BO->getLHS()->IgnoreParenCasts());
            if (DRE)
                if (const auto* VD = dyn_cast<VarDecl>(DRE->getDecl()))
                    MovedFrom.erase(VD);
        }
        return true;
    }

    // ── CXXConstructExpr: detect move construction AND check args for uses ────
    bool TraverseCXXConstructExpr(CXXConstructExpr* CE) {
        if (CE->getConstructor()->isMoveConstructor() && CE->getNumArgs() >= 1) {
            const Expr* Arg = CE->getArg(0)->IgnoreParenImpCasts();
            // Direct variable bound to move ctor (e.g. T(rval_var)):
            if (const auto* DRE = dyn_cast<DeclRefExpr>(Arg)) {
                if (const auto* VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                    if (isTrackableMovedObject(VD) &&
                        !hasSpecifiedMovedFromState(VD)) {
                        recordMove(VD, CE->getBeginLoc());
                        return true;
                    }
                }
            }
            // Argument is std::move(var) or static_cast<T&&>(var) — the
            // move will be detected when the child CallExpr/CastExpr is
            // traversed below.  Fall through to normal recursion.
        }

        // Check ALL constructor arguments for use of moved-from variables.
        // This catches e.g. copy-construction from a moved-from object.
        for (unsigned I = 0; I < CE->getNumArgs(); ++I)
            checkUse(CE->getArg(I));

        return RecursiveASTVisitor::TraverseCXXConstructExpr(CE);
    }

private:
    // Check if E refers to a moved-from variable and report a violation.
    void checkUse(const Expr* E) {
        if (!E) return;
        const Expr* Inner = E->IgnoreParenImpCasts();
        const auto* DRE = dyn_cast<DeclRefExpr>(Inner);
        if (!DRE) return;
        const auto* VD = dyn_cast<VarDecl>(DRE->getDecl());
        if (!VD) return;

        auto It = MovedFrom.find(VD);
        if (It == MovedFrom.end()) return;

        SourceLocation UseLoc = Ctx.getSourceManager().getSpellingLoc(
            DRE->getBeginLoc());
        if (!ReportedUses.insert(UseLoc.getRawEncoding()).second) return;

        Check.diag(UseLoc,
                   "%0 used while in a potentially moved-from state "
                   "(moved from here)")
            << VD;
        Check.diag(It->second.Location, "move occurred here",
                   DiagnosticIDs::Note);
    }

};

// ─── Check registration ─────────────────────────────────────────────────────

void SdcMovedFromStateCheck::registerMatchers(MatchFinder* Finder) {
    // Match every non-system-header function definition.
    // The visitor does the data-flow analysis inside each function body.
    Finder->addMatcher(
        functionDecl(
            isDefinition(),
            unless(isExpansionInSystemHeader())
        ).bind("func"),
        this
    );
}

void SdcMovedFromStateCheck::check(const MatchFinder::MatchResult& Result) {
    const auto* FD = Result.Nodes.getNodeAs<FunctionDecl>("func");
    // Defaulted bodies are synthesized by Clang. Their memberwise moves use
    // repeated xvalue casts of the enclosing parameter to access distinct
    // subobjects, not repeated source-level moves of that whole parameter.
    if (!FD || FD->isDependentContext() || FD->isDefaulted()) return;

    MovedFromVisitor V(*Result.Context, *this);
    V.run(FD);
}

} // namespace sdc
} // namespace tidy
} // namespace clang
