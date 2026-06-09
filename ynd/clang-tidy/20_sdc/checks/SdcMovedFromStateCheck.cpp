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
        StringRef N = ULE->getName().getAsString();
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

// Returns true if VD has type std::unique_ptr<...> (exempt from rule).
static bool isUniquePtr(const VarDecl* VD) {
    QualType T = VD->getType().getCanonicalType();
    const auto* RD = T->getAsCXXRecordDecl();
    if (!RD) return false;
    if (RD->getName() != "unique_ptr") return false;
    const auto* NS = dyn_cast<NamespaceDecl>(RD->getDeclContext());
    return NS && NS->isStdNamespace();
}

// ─── Per-function visitor ────────────────────────────────────────────────────

class MovedFromVisitor : public RecursiveASTVisitor<MovedFromVisitor> {
    ASTContext& Ctx;
    ClangTidyCheck& Check;

    // VD → source location of the move that put it in the moved-from state.
    llvm::DenseMap<const VarDecl*, SourceLocation> MovedFrom;

    // Guards against recursive re-entry while processing the argument of a move.
    bool InMoveArg = false;

public:
    MovedFromVisitor(ASTContext& C, ClangTidyCheck& Ch) : Ctx(C), Check(Ch) {}

    void run(const FunctionDecl* FD) {
        if (const Stmt* Body = FD->getBody())
            TraverseStmt(const_cast<Stmt*>(Body));

        // End-of-function: lvalue-ref params still in moved-from state are violations.
        reportLvalRefAtExit(FD);
    }

    // ── CallExpr: detect std::move/forward and use-of-moved-from ─────────────
    bool TraverseCallExpr(CallExpr* CE) {
        if (isStdMoveOrForward(CE)) {
            const VarDecl* VD = getMovedVarDecl(CE);
            if (VD && !isUniquePtr(VD)) {
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
            if (VD && !isUniquePtr(VD)) {
                MovedFrom[VD] = SC->getBeginLoc();
                return true; // Don't recurse into operand
            }
        }
        return RecursiveASTVisitor::TraverseCXXStaticCastExpr(SC);
    }

    // ── Member calls: check implicit object for moved-from state ─────────────
    bool TraverseCXXMemberCallExpr(CXXMemberCallExpr* MCE) {
        checkUse(MCE->getImplicitObjectArgument());
        return RecursiveASTVisitor::TraverseCXXMemberCallExpr(MCE);
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
                    if (!isUniquePtr(VD)) {
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

    // ── ReturnStmt: check lvalue-ref params ───────────────────────────────────
    bool VisitReturnStmt(ReturnStmt*) {
        for (const auto& [VD, MoveLoc] : MovedFrom) {
            if (const auto* PD = dyn_cast<ParmVarDecl>(VD))
                if (PD->getType()->isLValueReferenceType())
                    emitLvalRefReturn(PD, MoveLoc);
        }
        return true;
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

        Check.diag(DRE->getBeginLoc(),
                   "%0 used while in a potentially moved-from state "
                   "(moved from here)")
            << VD;
        Check.diag(It->second, "move occurred here",
                   DiagnosticIDs::Note);
    }

    void emitLvalRefReturn(const ParmVarDecl* PD, SourceLocation MoveLoc) {
        Check.diag(PD->getLocation(),
                   "lvalue reference parameter %0 is in a potentially "
                   "moved-from state when the function returns")
            << PD;
        Check.diag(MoveLoc, "move occurred here", DiagnosticIDs::Note);
    }

    void reportLvalRefAtExit(const FunctionDecl* FD) {
        // Triggered when a void function (or last-statement function) exits
        // without an explicit return but a param is still in moved-from state.
        if (!FD->getReturnType()->isVoidType()) return;
        for (const auto& [VD, MoveLoc] : MovedFrom) {
            if (const auto* PD = dyn_cast<ParmVarDecl>(VD))
                if (PD->getType()->isLValueReferenceType())
                    emitLvalRefReturn(PD, MoveLoc);
        }
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

    // Skip implicit instantiations — warn on the template pattern only.
    auto TSK = FD->getTemplateSpecializationKind();
    if (TSK == TSK_ImplicitInstantiation ||
        TSK == TSK_ExplicitInstantiationDefinition ||
        TSK == TSK_ExplicitInstantiationDeclaration)
        return;

    MovedFromVisitor V(*Result.Context, *this);
    V.run(FD);
}

} // namespace sdc
} // namespace tidy
} // namespace clang
