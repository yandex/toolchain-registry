#include "SdcLocalEvidenceUtils.h"
#include "clang/AST/ExprConcepts.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/Analysis/Analyses/ExprMutationAnalyzer.h"

namespace clang::tidy::sdc::local_evidence {
bool constantBool(const Expr *E, ASTContext &C, bool &Value) {
    if (!E || E->isValueDependent() || E->HasSideEffects(C)) return false;
    auto V = E->getIntegerConstantExpr(C);
    if (!V) return false;
    Value = *V != 0;
    return true;
}
const VarDecl *directObject(const Expr *E) {
    if (!E) return nullptr;
    const auto *R = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
    const auto *V = R ? dyn_cast<VarDecl>(R->getDecl()) : nullptr;
    return V && !V->getType()->isReferenceType() ? V : nullptr;
}
bool visibleEvaluation(const Expr *E, ASTContext &C) {
    if (ExprMutationAnalyzer::isUnevaluated(E, C)) return false;
    auto N = DynTypedNode::create(*E);
    for (unsigned Depth = 0; Depth < 128; ++Depth) {
        auto Parents = C.getParents(N);
        if (Parents.empty()) return true;
        if (Parents.size() != 1) return false; // Ambiguous shared AST ownership.
        const auto &P = Parents[0];
        bool V;
        const auto *Child = N.get<Stmt>();
        if (const auto *I = P.get<IfStmt>()) {
            if (I->isConstexpr()) {
                auto Selected = I->getNondiscardedCase(C);
                if (!Selected || (Child != *Selected &&
                    (Child == I->getThen() || Child == I->getElse()))) return false;
            } else if (constantBool(I->getCond(), C, V) &&
                       (Child == (V ? I->getElse() : I->getThen()))) return false;
        }
        if (const auto *B = P.get<BinaryOperator>())
            if (Child == B->getRHS() && constantBool(B->getLHS(), C, V) &&
                ((B->getOpcode() == BO_LAnd && !V) || (B->getOpcode() == BO_LOr && V))) return false;
        if (const auto *Q = P.get<ConditionalOperator>())
            if (constantBool(Q->getCond(), C, V) &&
                Child == (V ? Q->getFalseExpr() : Q->getTrueExpr())) return false;
        if (P.get<FunctionDecl>() || P.get<LambdaExpr>()) return true;
        N = P;
    }
    return false;
}
void walk(const Stmt *S, ASTContext &C, const std::function<void(const Stmt *)> &Visit,
          bool LinearOnly) {
    if (!S || isa<UnaryExprOrTypeTraitExpr, CXXNoexceptExpr, TypeTraitExpr,
                  RequiresExpr, ArrayTypeTraitExpr, ExpressionTraitExpr>(S)) return;
    auto Recurse = [&](const Stmt *Child) { walk(Child, C, Visit, LinearOnly); };
    if (const auto *A = dyn_cast<CXXDefaultArgExpr>(S)) { Recurse(A->getExpr()); return; }
    if (const auto *I = dyn_cast<CXXDefaultInitExpr>(S)) { Recurse(I->getExpr()); return; }
    if (const auto *L = dyn_cast<LambdaExpr>(S)) {
        for (const Expr *Init : L->capture_inits()) Recurse(Init);
        return;
    }
    if (const auto *T = dyn_cast<CXXTypeidExpr>(S)) {
        Visit(S);
        if (T->isPotentiallyEvaluated()) Recurse(T->getExprOperand());
        return;
    }
    if (const auto *Call = dyn_cast<CallExpr>(S))
        if (Call->isUnevaluatedBuiltinCall(C)) return;
    if (const auto *I = dyn_cast<IfStmt>(S)) {
        Recurse(I->getInit()); Recurse(I->getConditionVariableDeclStmt()); Recurse(I->getCond());
        if (I->isConstexpr()) {
            if (auto Selected = I->getNondiscardedCase(C)) Recurse(*Selected);
        } else {
            bool V;
            if (constantBool(I->getCond(), C, V)) Recurse(V ? I->getThen() : I->getElse());
            else if (!LinearOnly) { Recurse(I->getThen()); Recurse(I->getElse()); }
        }
        return;
    }
    if (const auto *B = dyn_cast<BinaryOperator>(S)) {
        if (B->isLogicalOp()) {
            Visit(S); Recurse(B->getLHS());
            bool V;
            if (constantBool(B->getLHS(), C, V)) {
                if ((B->getOpcode() == BO_LAnd) == V) Recurse(B->getRHS());
            } else if (!LinearOnly) Recurse(B->getRHS());
            return;
        }
    }
    if (const auto *Q = dyn_cast<ConditionalOperator>(S)) {
        Recurse(Q->getCond());
        bool V;
        if (constantBool(Q->getCond(), C, V)) Recurse(V ? Q->getTrueExpr() : Q->getFalseExpr());
        else if (!LinearOnly) { Recurse(Q->getTrueExpr()); Recurse(Q->getFalseExpr()); }
        return;
    }
    if (LinearOnly && isa<ForStmt, WhileStmt, DoStmt, CXXForRangeStmt, SwitchStmt, CXXTryStmt>(S)) return;
    Visit(S);
    if (const auto *D = dyn_cast<DeclStmt>(S)) {
        for (const Decl *Declaration : D->decls())
            if (const auto *V = dyn_cast<VarDecl>(Declaration)) Recurse(V->getInit());
        return;
    }
    for (const auto *Child : S->children()) {
        Recurse(Child);
        if (isa<CompoundStmt>(S) && Child &&
            isa<ReturnStmt, CXXThrowExpr, GotoStmt, BreakStmt, ContinueStmt>(Child)) break;
        // A branch could exit before the next statement. Do not infer that a
        // following call belongs to an unconditional path.
        if (LinearOnly && isa<CompoundStmt>(S) && Child &&
            isa<IfStmt, SwitchStmt, CXXTryStmt, ForStmt, WhileStmt, DoStmt>(Child)) break;
    }
}
} // namespace clang::tidy::sdc::local_evidence
