#include "SdcUnreachableCodeCheck.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/Type.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Analysis/Analyses/ReachableCode.h"
#include "clang/Analysis/CFG.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseSet.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

namespace {

// Collects every Stmt* that the rule considers exempt from the
// "unreachable" treatment even when clang's CFG marks it unreachable.
// Two categories are folded together because both produce the same
// "skip this CFG block" decision downstream:
//
//   * Statements inside the *discarded* branch of an `if constexpr`
//     whose condition is a known constant — the rule's closing
//     sentence explicitly excludes them.
//   * Operand sub-expressions of `&&`, `||`, and `?:` —
//     all operands of a reachable such operator are
//     themselves considered reachable. This matters most for template
//     instantiations where the short-circuited side is "dead" only in
//     some instantiations (e.g. `if constexpr` is the canonical form,
//     but `kFlag && f()` shows up too); clang's stock
class ExemptStmtCollector
    : public RecursiveASTVisitor<ExemptStmtCollector> {
public:
    explicit ExemptStmtCollector(ASTContext& Ctx) : Ctx_(Ctx) {}

    llvm::DenseSet<const Stmt*> Exempt;

    bool VisitIfStmt(IfStmt* If) {
        if (!If->isConstexpr()) {
            return true;
        }
        const Expr* Cond = If->getCond();
        if (!Cond) {
            return true;
        }
        bool Value = false;
        if (!Cond->EvaluateAsBooleanCondition(Value, Ctx_)) {
            return true;
        }
        const Stmt* Discarded = Value ? If->getElse() : If->getThen();
        if (Discarded) {
            collectAll(Discarded);
        }
        return true;
    }

    bool VisitBinaryOperator(BinaryOperator* B) {
        if (B->getOpcode() == BO_LAnd || B->getOpcode() == BO_LOr) {
            collectAll(B->getRHS());
        }
        return true;
    }

    bool VisitConditionalOperator(ConditionalOperator* C) {
        collectAll(C->getTrueExpr());
        collectAll(C->getFalseExpr());
        return true;
    }

    bool VisitBinaryConditionalOperator(BinaryConditionalOperator* C) {
        // GNU `a ?: b` — the true-expr aliases the condition (always
        // evaluated); only the false-expr can be short-circuited away.
        collectAll(C->getFalseExpr());
        return true;
    }

    bool VisitCXXForRangeStmt(CXXForRangeStmt* S) {
        // Exempt implicit iterator operations that the CFG builder sometimes
        // erroneously marks unreachable (especially with EHEdges enabled for
        // complex views), or which are genuinely unreachable if the loop
        // unconditionally exits. These are compiler-generated.
        if (S->getBeginStmt()) collectAll(S->getBeginStmt());
        if (S->getEndStmt()) collectAll(S->getEndStmt());
        if (S->getCond()) collectAll(S->getCond());
        if (S->getInc()) collectAll(S->getInc());
        return true;
    }

private:
    void collectAll(const Stmt* S) {
        if (!S) {
            return;
        }
        Exempt.insert(S);
        for (const Stmt* Child : S->children()) {
            collectAll(Child);
        }
    }

    ASTContext& Ctx_;
};

const Stmt* firstStmtWithLoc(const CFGBlock* B) {
    for (const auto& E : *B) {
        if (auto SE = E.getAs<CFGStmt>()) {
            const Stmt* S = SE->getStmt();
            if (S && S->getBeginLoc().isValid()) {
                return S;
            }
        }
    }
    return nullptr;
}

// Strip reference/pointer/cv to get at the underlying record type for
// catch-handler subsumption checks. `catch(T)`, `catch(T&)`, `catch(const T&)`
// all match the same set of thrown types relative to the type hierarchy.
QualType getCaughtRecordType(QualType T) {
    T = T.getNonReferenceType();
    if (const auto* PT = T->getAs<PointerType>()) {
        T = PT->getPointeeType();
    }
    return T.getUnqualifiedType();
}

// True if handler with type `Earlier` would catch anything that handler with
// type `Later` could catch — i.e., Later is unreachable when Earlier precedes
// it in the same try.
bool earlierSubsumesLater(QualType Earlier, QualType Later) {
    QualType E = getCaughtRecordType(Earlier);
    QualType L = getCaughtRecordType(Later);
    if (E.isNull() || L.isNull()) {
        return false;
    }
    // Exact type match - duplicate handler, the second is dead.
    if (E.getCanonicalType() == L.getCanonicalType()) {
        return true;
    }
    const auto* ERecord = E->getAsCXXRecordDecl();
    const auto* LRecord = L->getAsCXXRecordDecl();
    if (!ERecord || !LRecord) {
        return false;
    }
    if (!ERecord->hasDefinition() || !LRecord->hasDefinition()) {
        return false;
    }
    // If Later derives from Earlier, an Earlier-typed handler catches Later.
    return LRecord->isDerivedFrom(ERecord);
}

// Locates each catch handler that is unreachable because an earlier handler
// in the same try-block already catches everything this one would.
// `catch(...)` subsumes every following handler.
class UnreachableCatchFinder
    : public RecursiveASTVisitor<UnreachableCatchFinder> {
public:
    llvm::SmallVector<const CXXCatchStmt*> Unreachable;

    bool VisitCXXTryStmt(CXXTryStmt* Try) {
        unsigned N = Try->getNumHandlers();
        for (unsigned i = 1; i < N; ++i) {
            const CXXCatchStmt* Later = Try->getHandler(i);
            for (unsigned j = 0; j < i; ++j) {
                const CXXCatchStmt* Earlier = Try->getHandler(j);
                if (subsumes(Earlier, Later)) {
                    Unreachable.push_back(Later);
                    break;
                }
            }
        }
        return true;
    }

private:
    static bool subsumes(const CXXCatchStmt* Earlier, const CXXCatchStmt* Later) {
        // catch(...) - null exception decl - catches everything.
        if (!Earlier->getExceptionDecl()) {
            return true;
        }
        if (!Later->getExceptionDecl()) {
            return false;
        }
        return earlierSubsumesLater(
            Earlier->getCaughtType(), Later->getCaughtType());
    }
};

} // namespace

SdcUnreachableCodeCheck::SdcUnreachableCodeCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context)
{
}

void SdcUnreachableCodeCheck::registerPPCallbacks(
    const SourceManager& /*SM*/, Preprocessor* PP, Preprocessor* /*ModuleExpanderPP*/) {
    PP_ = PP;
}

void SdcUnreachableCodeCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        functionDecl(
            isDefinition(),
            unless(isExpansionInSystemHeader()))
            .bind("fn"),
        this);
}

void SdcUnreachableCodeCheck::check(const MatchFinder::MatchResult& Result) {
    const auto* FD = Result.Nodes.getNodeAs<FunctionDecl>("fn");
    if (!FD || !FD->hasBody()) {
        return;
    }
    // Uninstantiated templates have no concrete CFG to analyze; each
    // concrete instantiation is matched separately and handled there.
    if (FD->isDependentContext()) {
        return;
    }
    if (FD != FD->getDefinition()) {
        return;
    }

    AnalysisDeclContextManager Mgr(*Result.Context);
    // EH edges let us see catch handlers as unreachable when the try body
    // has no potentially-throwing statement.
    Mgr.getCFGBuildOptions().AddEHEdges = true;
    AnalysisDeclContext* AC = Mgr.getContext(FD);
    if (!AC) {
        return;
    }
    const CFG* Cfg = AC->getCFG();
    if (!Cfg) {
        return;
    }

    ExemptStmtCollector Collector(*Result.Context);
    Collector.TraverseStmt(const_cast<Stmt*>(FD->getBody()));

    UnreachableCatchFinder CatchFinder;
    CatchFinder.TraverseStmt(const_cast<Stmt*>(FD->getBody()));
    for (const CXXCatchStmt* C : CatchFinder.Unreachable) {
        diag(C->getBeginLoc(), "unreachable code");
    }

    llvm::BitVector Reachable(Cfg->getNumBlockIDs());
    reachable_code::ScanReachableFromBlock(&Cfg->getEntry(), Reachable);

    // Dedup by file offset: a single source statement can occupy multiple
    // CFG elements/blocks, and we don't want N copies of the same warning.
    llvm::DenseSet<unsigned> Reported;

    for (const CFGBlock* B : *Cfg) {
        if (!B) {
            continue;
        }
        if (Reachable[B->getBlockID()]) {
            continue;
        }
        if (B == &Cfg->getEntry() || B == &Cfg->getExit()) {
            continue;
        }
        const Stmt* S = firstStmtWithLoc(B);
        if (!S) {
            continue;
        }
        if (Collector.Exempt.count(S)) {
            continue;
        }
        SourceLocation L = S->getBeginLoc();
        if (L.isInvalid()) {
            continue;
        }
        unsigned Off = Result.SourceManager->getFileOffset(
            Result.SourceManager->getExpansionLoc(L));
        if (!Reported.insert(Off).second) {
            continue;
        }
        diag(L, "unreachable code");
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
