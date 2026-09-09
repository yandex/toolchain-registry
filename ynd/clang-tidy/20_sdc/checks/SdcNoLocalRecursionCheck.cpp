#include "SdcNoLocalRecursionCheck.h"
#include "SdcLocalEvidenceUtils.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include <map>
#include <set>
#include <vector>

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
namespace {
struct Edge { const FunctionDecl *To; const CallExpr *Call; };
class Inventory : public RecursiveASTVisitor<Inventory> {
public:
    explicit Inventory(ASTContext &C) : C(C) {}
    bool shouldVisitTemplateInstantiations() const { return true; }
    bool VisitFunctionDecl(FunctionDecl *F) {
        if (!F->doesThisDeclarationHaveABody() || F->isInvalidDecl() || F->isConstexpr() ||
            !isInAnalyzedCode(*F, F->getLocation(), C)) return true;
        auto &Edges = Graph[F->getCanonicalDecl()];
        bool PastCall = false;
        local_evidence::walk(F->getBody(), C, [&](const Stmt *S) {
            if (PastCall) return;
            if (isa<CXXConstructExpr, CXXNewExpr, CXXDeleteExpr, AsmStmt>(S)) {
                PastCall = true;
                return;
            }
            const auto *Call = dyn_cast<CallExpr>(S);
            if (!Call) return;
            // A previous call might never return, particularly when its body
            // is unavailable. Keep only the first call in this limited prefix.
            PastCall = true;
            const auto *Callee = Call ? Call->getDirectCallee() : nullptr;
            if (!Callee || !Callee->hasBody() || Callee->isInvalidDecl()) return;
            for (const Expr *Argument : Call->arguments())
                if (Argument->HasSideEffects(C)) return;
            // A statically named virtual method need not be the runtime target.
            if (const auto *M = dyn_cast<CXXMethodDecl>(Callee))
                if (M->isVirtual()) return;
            Edges.push_back({Callee->getCanonicalDecl(), Call});
        }, true);
        return true;
    }
    std::map<const FunctionDecl *, std::vector<Edge>> Graph;
private:
    ASTContext &C;
};
} // namespace
void SdcNoLocalRecursionCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(translationUnitDecl().bind("tu"), this);
}
void SdcNoLocalRecursionCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    Inventory I(C); I.TraverseDecl(C.getTranslationUnitDecl());
    // This fast check deliberately yields no findings on oversized graphs.
    // It never converts an exhausted analysis budget into a violation.
    if (I.Graph.size() > 4096) return;
    std::map<const FunctionDecl *, unsigned> Index, Low, Component;
    std::set<const FunctionDecl *> OnStack;
    std::vector<const FunctionDecl *> Stack;
    unsigned Next = 0, Group = 0;
    std::function<void(const FunctionDecl *)> Connect = [&](const FunctionDecl *F) {
        Index[F] = Low[F] = ++Next;
        Stack.push_back(F); OnStack.insert(F);
        for (const auto &E : I.Graph.at(F)) {
            if (!I.Graph.count(E.To)) continue;
            if (!Index.count(E.To)) { Connect(E.To); Low[F] = std::min(Low[F], Low[E.To]); }
            else if (OnStack.count(E.To)) Low[F] = std::min(Low[F], Index[E.To]);
        }
        if (Low[F] != Index[F]) return;
        ++Group;
        const FunctionDecl *P;
        do {
            P = Stack.back(); Stack.pop_back(); OnStack.erase(P); Component[P] = Group;
        } while (P != F);
    };
    for (const auto &[F, Edges] : I.Graph) if (!Index.count(F)) Connect(F);
    for (const auto &[F, Edges] : I.Graph) {
        for (const auto &E : Edges) {
            if (!Component.count(E.To) || Component[F] != Component[E.To]) continue;
            auto L = E.Call->getExprLoc();
            if (!isInAnalyzedCode(*E.Call, L, C)) continue;
            for (const Decl *Instance : Instances.claim(*E.Call, L, C)) {
                diagnoseAnalysisInstance(*this, Instance, C, L,
                    "this direct call participates in a locally resolved recursion cycle");
                diag(E.To->getLocation(), "cycle calls this function", DiagnosticIDs::Note);
            }
            break; // One actionable call per participating function.
        }
    }
}
} // namespace clang::tidy::sdc
