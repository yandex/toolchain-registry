#include "SdcMovedFromStateCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"

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
        return D && D->getName() == N;
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
    if (!MD || MD->getName() != "clear" || MD->getNumParams() != 0)
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
    if (RD->getName() != "unique_ptr" && RD->getName() != "shared_ptr")
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

// Whether control may continue elsewhere in the enclosing function after S.
// Only a return is unconditionally terminal for this lightweight analysis.
// Other control transfers need a CFG to route their state to the correct
// destination, so retain their state conservatively rather than risk a miss.
static bool canFallThrough(const Stmt* S) {
    if (!S) return true;
    if (isa<ReturnStmt>(S)) return false;

    if (const auto* Compound = dyn_cast<CompoundStmt>(S)) {
        if (Compound->body_empty()) return true;
        return canFallThrough(Compound->body_back());
    }

    if (const auto* Attributed = dyn_cast<AttributedStmt>(S))
        return canFallThrough(Attributed->getSubStmt());

    if (const auto* If = dyn_cast<IfStmt>(S)) {
        if (!If->getElse()) return true;
        return canFallThrough(If->getThen()) ||
               canFallThrough(If->getElse());
    }

    return true;
}

// ─── Per-function visitor ────────────────────────────────────────────────────

class MovedFromVisitor : public RecursiveASTVisitor<MovedFromVisitor> {
    ASTContext& Ctx;
    ClangTidyCheck& Check;

    // VD → source location of the move that put it in the moved-from state.
    llvm::DenseMap<const VarDecl*, SourceLocation> MovedFrom;

    // Loop re-traversal deliberately visits some AST nodes more than once.
    // Keep diagnostics unique by spelling location.
    llvm::DenseSet<unsigned> ReportedUses;

    // Guards against recursive re-entry while processing the argument of a move.
    bool InMoveArg = false;

public:
    MovedFromVisitor(ASTContext& C, ClangTidyCheck& Ch) : Ctx(C), Check(Ch) {}

    void run(const FunctionDecl* FD) {
        if (const Stmt* Body = FD->getBody())
            TraverseStmt(const_cast<Stmt*>(Body));
    }

    // ── CallExpr: detect std::move/forward and use-of-moved-from ─────────────
    bool TraverseCallExpr(CallExpr* CE) {
        if (isStdMoveOrForward(CE)) {
            const VarDecl* VD = getMovedVarDecl(CE);
            if (!onlyBindsReference(CE, Ctx) &&
                isTrackableMovedObject(VD) &&
                !hasSpecifiedMovedFromState(VD)) {
                if (MovedFrom.count(VD) != 0 && CE->getNumArgs() != 0)
                    checkUse(CE->getArg(0));
                MovedFrom[VD] = CE->getBeginLoc();
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
                MovedFrom[VD] = SC->getBeginLoc();
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
        if (OC->getOperator() != OO_Equal)
            for (const Expr* Arg : OC->arguments()) checkUse(Arg);
        return RecursiveASTVisitor::TraverseCXXOperatorCallExpr(OC);
    }

    // The branches of an if statement are mutually exclusive.  Traverse each
    // from the state established by the condition, then conservatively join
    // the two possible exit states.  A single linear AST traversal would make
    // a move in the then branch visible while inspecting the else branch.
    bool TraverseIfStmt(IfStmt* S) {
        if (Stmt* Init = S->getInit())
            if (!TraverseStmt(Init)) return false;
        if (VarDecl* ConditionVar = S->getConditionVariable())
            if (!TraverseDecl(ConditionVar)) return false;
        if (!TraverseStmt(S->getCond())) return false;

        const auto EntryState = MovedFrom;

        if (!TraverseStmt(S->getThen())) return false;
        const auto ThenState = MovedFrom;
        const bool ThenFallsThrough = canFallThrough(S->getThen());

        MovedFrom = EntryState;
        if (Stmt* Else = S->getElse())
            if (!TraverseStmt(Else)) return false;
        const auto ElseState = MovedFrom;
        const bool ElseFallsThrough = canFallThrough(S->getElse());

        MovedFrom.clear();
        if (ThenFallsThrough)
            for (const auto& [VD, MoveLoc] : ThenState)
                MovedFrom.try_emplace(VD, MoveLoc);
        if (ElseFallsThrough)
            for (const auto& [VD, MoveLoc] : ElseState)
                MovedFrom.try_emplace(VD, MoveLoc);
        return true;
    }

    // A value moved at the end of a loop body is still moved-from at the
    // beginning of the next iteration.  A second traversal is a bounded,
    // conservative back-edge approximation suitable for this STU check.
    bool TraverseForStmt(ForStmt* S) {
        if (!RecursiveASTVisitor::TraverseForStmt(S)) return false;
        return TraverseStmt(S->getBody());
    }

    bool TraverseCXXForRangeStmt(CXXForRangeStmt* S) {
        if (!RecursiveASTVisitor::TraverseCXXForRangeStmt(S)) return false;
        // The loop variable is rebound to the next element on every
        // iteration.  It does not retain the moved-from state of the object
        // denoted by the previous iteration's binding.
        MovedFrom.erase(S->getLoopVariable());
        return TraverseStmt(S->getBody());
    }

    bool TraverseWhileStmt(WhileStmt* S) {
        if (!RecursiveASTVisitor::TraverseWhileStmt(S)) return false;
        return TraverseStmt(S->getBody());
    }

    bool TraverseDoStmt(DoStmt* S) {
        if (!RecursiveASTVisitor::TraverseDoStmt(S)) return false;
        return TraverseStmt(S->getBody());
    }

    // Each execution of a declaration creates a fresh object.  This is
    // especially important during the bounded second traversal of loop
    // bodies: a variable declared inside the body is not moved-from merely
    // because the previous iteration's distinct object was moved.
    bool VisitVarDecl(VarDecl* VD) {
        MovedFrom.erase(VD);
        return true;
    }

    // ── Binary operator: assignment clears moved-from state ──────────────────
    bool VisitBinaryOperator(BinaryOperator* BO) {
        if (BO->isAssignmentOp()) {
            const auto* DRE = dyn_cast<DeclRefExpr>(
                BO->getLHS()->IgnoreParenCasts());
            if (DRE)
                if (const auto* VD = dyn_cast<VarDecl>(DRE->getDecl()))
                    MovedFrom.erase(VD);
        }
        return true;
    }

    // CXXOperatorCallExpr for operator=
    bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr* OC) {
        if (OC->getOperator() == OO_Equal && OC->getNumArgs() >= 1) {
            const auto* DRE = dyn_cast<DeclRefExpr>(
                OC->getArg(0)->IgnoreParenCasts());
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
                        MovedFrom[VD] = CE->getBeginLoc();
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
        Check.diag(It->second, "move occurred here",
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
    if (!FD || FD->isDependentContext()) return;

    MovedFromVisitor V(*Result.Context, *this);
    V.run(FD);
}

} // namespace sdc
} // namespace tidy
} // namespace clang
