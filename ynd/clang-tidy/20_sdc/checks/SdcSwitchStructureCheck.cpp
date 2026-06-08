#include "SdcSwitchStructureCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/SmallVector.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcSwitchStructureCheck::SdcSwitchStructureCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcSwitchStructureCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        switchStmt(unless(isExpansionInSystemHeader())).bind("sw"),
        this);
}

namespace {

// ── Termination helpers ────────────────────────────────────────────────────

// Returns true if E is a call to a [[noreturn]] (or _Noreturn) function.
bool isNoReturnCall(const Expr* E) {
    E = E->IgnoreImpCasts();
    if (const auto* EWC = dyn_cast<ExprWithCleanups>(E))
        E = EWC->getSubExpr()->IgnoreImpCasts();
    if (const auto* CE = dyn_cast<CallExpr>(E)) {
        if (const FunctionDecl* FD = CE->getDirectCallee())
            return FD->isNoReturn();
    }
    return false;
}

// Returns true when S (or the last statement it contains) unconditionally
// exits control: break, continue, return, goto, throw, [[noreturn]] call,
// or [[fallthrough]].  Recursively descends into compound and if-else nodes.
bool isUnconditionalTerminator(const Stmt* S) {
    if (!S) return false;

    if (isa<BreakStmt>(S) || isa<ContinueStmt>(S) ||
        isa<ReturnStmt>(S) || isa<GotoStmt>(S))
        return true;

    if (isa<CXXThrowExpr>(S)) return true;

    // [[fallthrough]]; — AttributedStmt wrapping a NullStmt
    if (const auto* AS = dyn_cast<AttributedStmt>(S)) {
        for (const auto* Attr : AS->getAttrs())
            if (isa<FallThroughAttr>(Attr)) return true;
        // Also check the inner statement for other terminator patterns
        return isUnconditionalTerminator(AS->getSubStmt());
    }

    // Clang wraps expressions that need cleanup (e.g. throw with a temporary
    // argument) in ExprWithCleanups.  Unwrap to reach the actual expression.
    if (const auto* EWC = dyn_cast<ExprWithCleanups>(S))
        return isUnconditionalTerminator(EWC->getSubExpr());

    // Call to [[noreturn]] function used as a statement
    if (const auto* E = dyn_cast<Expr>(S))
        if (isNoReturnCall(E)) return true;

    // User-defined label wrapping another statement (e.g. myLabel: break;)
    if (const auto* LS = dyn_cast<LabelStmt>(S))
        return isUnconditionalTerminator(LS->getSubStmt());

    // Compound statement — last child must terminate
    if (const auto* CS = dyn_cast<CompoundStmt>(S))
        return !CS->body_empty() &&
               isUnconditionalTerminator(CS->body_back());

    // if-else where BOTH branches terminate
    if (const auto* IS = dyn_cast<IfStmt>(S))
        return IS->getElse() &&
               isUnconditionalTerminator(IS->getThen()) &&
               isUnconditionalTerminator(IS->getElse());

    return false;
}

// Walk a label chain (case/default nested as substatements) and return the
// deepest non-label substatement, or null if the chain is empty.
const Stmt* unwrapLabelChain(const Stmt* S) {
    while (S) {
        if (const auto* CS = dyn_cast<CaseStmt>(S))  { S = CS->getSubStmt(); continue; }
        if (const auto* DS = dyn_cast<DefaultStmt>(S)){ S = DS->getSubStmt(); continue; }
        break;
    }
    return S;
}

// Returns the "effective last statement" that determines whether a branch
// terminates.  BranchStmts is the flat list of CompoundStmt children that
// belong to this branch (starting with the leading case/default label).
const Stmt* effectiveLastStmt(
    const llvm::SmallVector<const Stmt*, 8>& Branch) {

    if (Branch.empty()) return nullptr;
    const Stmt* Last = Branch.back();

    // If the last element of the branch is itself a case/default label
    // (happens when the branch has no code after the label chain, e.g.
    // `case 1: case 2:` with only the inner chain and no siblings),
    // unwrap the label chain to get the innermost body.
    if (isa<CaseStmt>(Last) || isa<DefaultStmt>(Last))
        return unwrapLabelChain(Last);

    return Last;
}

// ── Default label helpers ─────────────────────────────────────────────────

// Returns true if S (or the start of its label chain) is or contains a
// DefaultStmt.
bool chainHasDefault(const Stmt* S) {
    while (S) {
        if (isa<DefaultStmt>(S)) return true;
        if (const auto* CS = dyn_cast<CaseStmt>(S)) { S = CS->getSubStmt(); continue; }
        break;
    }
    return false;
}

// Returns true if the DEFAULT is the FIRST label in the label chain of S.
bool defaultIsFirstInChain(const Stmt* S) {
    return isa<DefaultStmt>(S);
}

// Returns true if the DEFAULT is the LAST label in the label chain of S
// (i.e. the immediate predecessor of the actual code).
bool defaultIsLastInChain(const Stmt* S) {
    while (S) {
        if (const auto* DS = dyn_cast<DefaultStmt>(S)) {
            // default is here; check that its substatement is NOT a case/default
            const Stmt* Sub = DS->getSubStmt();
            return !Sub || (!isa<CaseStmt>(Sub) && !isa<DefaultStmt>(Sub));
        }
        if (const auto* CS = dyn_cast<CaseStmt>(S)) { S = CS->getSubStmt(); continue; }
        break;
    }
    return false;
}

// ── Enum coverage helper ──────────────────────────────────────────────────

// Returns true when all enumerators of ED have a matching case value.
bool allEnumeratorsCovered(
    const EnumDecl* ED, const SwitchStmt* SS, ASTContext& Ctx) {

    llvm::SmallVector<llvm::APSInt, 16> CaseVals;
    for (const SwitchCase* SC = SS->getSwitchCaseList(); SC;
         SC = SC->getNextSwitchCase()) {
        if (const auto* CS = dyn_cast<CaseStmt>(SC)) {
            if (const Expr* LHS = CS->getLHS()) {
                Expr::EvalResult R;
                if (LHS->EvaluateAsInt(R, Ctx))
                    CaseVals.push_back(R.Val.getInt());
            }
        }
    }
    for (const auto* ECD : ED->enumerators()) {
        bool found = false;
        for (const auto& CV : CaseVals) {
            if (CV == ECD->getInitVal()) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

} // namespace

void SdcSwitchStructureCheck::check(
    const MatchFinder::MatchResult& Result) {

    const auto* SS = Result.Nodes.getNodeAs<SwitchStmt>("sw");
    if (!SS) return;

    ASTContext& Ctx = *Result.Context;

    // ── Body shall be a compound statement ───────────────────────────────
    const auto* Body = dyn_cast_or_null<CompoundStmt>(SS->getBody());
    if (!Body) {
        diag(SS->getSwitchLoc(),
             "switch body shall be a compound statement");
        return;
    }

    llvm::SmallVector<const Stmt*, 16> Children(
        Body->body_begin(), Body->body_end());

    if (Children.empty()) {
        diag(SS->getSwitchLoc(),
             "switch statement shall have at least two branches");
        return;
    }

    // ── Sub-clause 4: first statement is a switch label group ─────────────
    if (!isa<CaseStmt>(Children[0]) && !isa<DefaultStmt>(Children[0])) {
        diag(Children[0]->getBeginLoc(),
             "first statement in switch body shall be a case or default label");
    }

    // ── Partition into branches ───────────────────────────────────────────
    using Branch = llvm::SmallVector<const Stmt*, 8>;
    llvm::SmallVector<Branch, 8> Branches;

    for (const Stmt* S : Children) {
        if (isa<CaseStmt>(S) || isa<DefaultStmt>(S))
            Branches.emplace_back();
        if (!Branches.empty())
            Branches.back().push_back(S);
    }

    // ── Sub-clause 6: at least two branches ──────────────────────────────
    if (Branches.size() < 2) {
        diag(SS->getSwitchLoc(),
             "switch statement shall have at least two branches");
    }

    // ── Sub-clause 3: label groups shall only contain case/default labels ─
    for (const Branch& Br : Branches) {
        const Stmt* S = Br.front();
        // Walk the label chain
        while (S) {
            if (isa<CaseStmt>(S)) { S = cast<CaseStmt>(S)->getSubStmt(); continue; }
            if (isa<DefaultStmt>(S)) { S = cast<DefaultStmt>(S)->getSubStmt(); continue; }
            if (isa<LabelStmt>(S)) {
                diag(S->getBeginLoc(),
                     "only case and default labels are permitted in a "
                     "switch label group");
            }
            break;
        }
    }

    // ── Sub-clause 5: every branch shall be unconditionally terminated ────
    for (const Branch& Br : Branches) {
        const Stmt* Last = effectiveLastStmt(Br);
        if (Last && !isUnconditionalTerminator(Last)) {
            diag(Br.front()->getBeginLoc(),
                 "switch branch shall be unconditionally terminated "
                 "(break, return, continue, goto, throw, [[noreturn]] call, "
                 "or [[fallthrough]])");
        }
    }

    // ── Sub-clause 7: default label required ──────────────────────────────
    // Locate the default (if any) and check its placement.
    int  defaultBranchIdx = -1;
    bool defaultIsFirst   = false;  // first label of its branch
    bool defaultIsLast    = false;  // last label of its branch

    for (int i = 0; i < (int)Branches.size(); ++i) {
        const Stmt* Head = Branches[i].front();
        if (chainHasDefault(Head)) {
            defaultBranchIdx = i;
            defaultIsFirst   = defaultIsFirstInChain(Head);
            defaultIsLast    = defaultIsLastInChain(Head);
            break;
        }
    }

    if (defaultBranchIdx == -1) {
        // No default — check enum coverage exception
        bool exceptionApplies = false;
        if (const Expr* Cond = SS->getCond()) {
            // The switch condition is typically promoted (e.g. enum→int);
            // strip implicit casts to reach the original enum type.
            QualType T = Cond->IgnoreImpCasts()->getType().getCanonicalType();
            if (const auto* ET = T->getAs<EnumType>()) {
                const EnumDecl* ED = ET->getDecl();
                if (!ED->isScoped() &&
                    ED->getIntegerTypeSourceInfo() == nullptr &&
                    allEnumeratorsCovered(ED, SS, Ctx))
                    exceptionApplies = true;
            }
        }
        if (!exceptionApplies) {
            diag(SS->getSwitchLoc(),
                 "switch statement shall have a default label");
        }
    } else {
        // Default exists — check placement: first label of first branch OR
        // last label of last branch.
        bool correctPlacement =
            (defaultBranchIdx == 0 && defaultIsFirst) ||
            (defaultBranchIdx == (int)Branches.size() - 1 && defaultIsLast);

        if (!correctPlacement) {
            diag(Branches[defaultBranchIdx].front()->getBeginLoc(),
                 "default label shall appear as the first label of the first "
                 "switch branch or the last label of the last switch branch");
        }
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
