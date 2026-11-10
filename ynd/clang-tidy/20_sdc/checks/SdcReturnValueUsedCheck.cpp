#include "SdcPolicyDiagnostic.h"
#include "SdcReturnValueUsedCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/Type.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/ExprConcepts.h"
#include "clang/AST/DeclTemplate.h"
#include <set>

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcReturnValueUsedCheck::SdcReturnValueUsedCheck(
    StringRef Name, ClangTidyContext* Context)
    : SdcPolicyCheck(Name, Context)
{
}

void SdcReturnValueUsedCheck::registerMatchers(MatchFinder* Finder) {
    // Match all call expressions (function calls, operator calls, member calls)
    Finder->addMatcher(
        callExpr(
            unless(hasParent(callExpr())),
            unless(hasParent(cxxMemberCallExpr())),
            unless(isExpansionInSystemHeader()))
            .bind("call_expr"),
        this);

    // Match C++ operator calls separately
    Finder->addMatcher(
        cxxOperatorCallExpr(
            unless(isExpansionInSystemHeader())).bind("operator_call"),
        this);

    // Match member calls separately
    Finder->addMatcher(
        cxxMemberCallExpr(
            unless(isExpansionInSystemHeader())).bind("member_call"),
        this);


}


void SdcReturnValueUsedCheck::check(const MatchFinder::MatchResult& Result) {
    // Synthesized special-member bodies contain calls the user did not write.
    for (const char* Binding : {"call_expr", "operator_call", "member_call"}) {
        if (const auto* Call = Result.Nodes.getNodeAs<CallExpr>(Binding)) {
            auto Node = DynTypedNode::create(*Call);
            while (true) {
                auto Parents = Result.Context->getParents(Node);
                if (Parents.empty()) break;
                Node = Parents[0];
                if (const auto* Function = Node.get<FunctionDecl>()) {
                    if (Function->isImplicit() || Function->isDefaulted()) return;
                    break;
                }
            }
        }
    }
    if (const auto* Call = Result.Nodes.getNodeAs<CallExpr>("call_expr")) {
        checkUnusedFunctionCall(Call, Result);
    } else if (const auto* OperatorCall = Result.Nodes.getNodeAs<CXXOperatorCallExpr>("operator_call")) {
        checkUnusedLambdaCall(OperatorCall, Result);
    } else if (const auto* MemberCall = Result.Nodes.getNodeAs<CXXMemberCallExpr>("member_call")) {
        checkUnusedMemberCall(MemberCall, Result);
    }
}

void SdcReturnValueUsedCheck::checkUnusedFunctionCall(
    const CallExpr* Call, const MatchFinder::MatchResult& Result) {
    // Skip void functions
    QualType ReturnType = Call->getCallReturnType(*Result.Context);
    if (ReturnType.isNull() || ReturnType->isVoidType()) {
        return;
    }

    if (clang::dyn_cast<CXXOperatorCallExpr>(Call)) {
        return; // Handled by checkUnusedLambdaCall
    }
    if (clang::isa<CXXMemberCallExpr>(Call)) {
        return; // Handled by checkUnusedMemberCall
    }

    // FIX: Removed the trap that bailed out if Parent was not a Stmt.
    if (!isReturnValueUsed(Call, Result)) {
        emitPolicyDiagnostic(*this, DynTypedNode::create(*Call), Call->getBeginLoc(),
             "return value of function call is not used; if intended to discard, cast to void", *Result.Context, AnalysisInstances);
    }
}

void SdcReturnValueUsedCheck::checkUnusedLambdaCall(
    const CXXOperatorCallExpr* OperatorCall, const MatchFinder::MatchResult& Result) {
    const Decl* CalleeDecl = OperatorCall->getCalleeDecl();
    if (!CalleeDecl) {
        return;
    }
    if (const auto* Method = clang::dyn_cast<CXXMethodDecl>(CalleeDecl)) {
        if (Method->getOverloadedOperator() == OO_Call) {
            if (!Method->getReturnType()->isVoidType()) {
                // FIX: Removed the Stmt cast trap
                if (!isReturnValueUsed(OperatorCall, Result)) {
                    emitPolicyDiagnostic(*this, DynTypedNode::create(*OperatorCall), OperatorCall->getBeginLoc(),
                         "return value of lambda call is not used; if intended to discard, cast to void", *Result.Context, AnalysisInstances);
                }
            }
        }
    }
}

void SdcReturnValueUsedCheck::checkUnusedMemberCall(
    const CXXMemberCallExpr* MemberCall, const MatchFinder::MatchResult& Result) {
    QualType ReturnType = MemberCall->getCallReturnType(*Result.Context);
    if (ReturnType.isNull() || ReturnType->isVoidType()) {
        return;
    }

    // FIX: Removed the Stmt cast trap
    if (!isReturnValueUsed(MemberCall, Result)) {
        emitPolicyDiagnostic(*this, DynTypedNode::create(*MemberCall), MemberCall->getBeginLoc(),
             "return value of member function call is not used; if intended to discard, cast to void", *Result.Context, AnalysisInstances);
    }
}

bool SdcReturnValueUsedCheck::isReturnValueUsed(
    const Expr* CallExpr, const MatchFinder::MatchResult& Result) {

    auto Parents = Result.Context->getParents(*CallExpr);
    if (Parents.empty()) {
        return false;
    }

    // A statement expression is an expression boundary, unlike a function
    // body. Even its non-final statements are unevaluated inside sizeof.
    auto UnevaluatedParents = Parents;
    while (!UnevaluatedParents.empty()) {
        const auto& Node = UnevaluatedParents[0];
        if (Node.get<UnaryExprOrTypeTraitExpr>() || Node.get<CXXNoexceptExpr>()) return true;
        if (const auto* Block = Node.get<CompoundStmt>()) {
            auto Outer = Result.Context->getParents(*Block);
            if (Outer.empty() || !Outer[0].get<StmtExpr>()) break;
        }
        if (Node.get<LambdaExpr>() || Node.get<FunctionDecl>()) break;
        const auto* S = Node.get<Stmt>();
        if (!S) break;
        UnevaluatedParents = Result.Context->getParents(*S);
    }
    auto StopsAtBlock = [&](const CompoundStmt* Block) {
        if (!Block) return false;
        auto Outer = Result.Context->getParents(*Block);
        return Outer.empty() || !Outer[0].get<StmtExpr>() || Block->body_empty() ||
               !isa<Expr>(Block->body_back()) ||
               !containsExpression(Block->body_back(), CallExpr);
    };

    // Check if the return value is explicitly discarded with void cast at the immediate level
    if (const auto* ImmediateStmt = Parents.begin()->get<Stmt>()) {
        if (isDiscardedCorrectly(ImmediateStmt)) {
            return true;
        }
    }

    auto UpwardParents = Parents;
    while (!UpwardParents.empty()) {
        if (const auto* MemberCall = UpwardParents.begin()->get<CXXMemberCallExpr>()) {
            if (const auto* ImplicitObj = MemberCall->getImplicitObjectArgument()) {
                if (containsExpression(ImplicitObj, CallExpr)) {
                    return true;
                }
            }
        }
        if (StopsAtBlock(UpwardParents.begin()->get<CompoundStmt>())) break;
        const auto* ParentStmt = UpwardParents.begin()->get<Stmt>();
        if (!ParentStmt) break;
        UpwardParents = Result.Context->getParents(*ParentStmt);
    }

    {
        auto DeclParents = Parents;
        while (!DeclParents.empty()) {
            const auto& ParentNode = *DeclParents.begin();
            if (const auto* Ctor = ParentNode.get<CXXConstructorDecl>()) {
                for (const auto* Init : Ctor->inits())
                    if (containsExpression(Init->getInit(), CallExpr)) return true;
            }
            if (StopsAtBlock(ParentNode.get<CompoundStmt>()) || ParentNode.get<FunctionDecl>()) break;

            if (const auto* VD = ParentNode.get<VarDecl>()) {
                return true;
            }
            if (const auto* FD = ParentNode.get<FieldDecl>()) {
                return true;
            }
            if (ParentNode.get<EnumConstantDecl>()) return true;
            if (const auto* CtorInit = ParentNode.get<CXXCtorInitializer>()) {
                return true;
            }

            if (ParentNode.get<TypeLoc>()) {
                return true;
            }

            if (ParentNode.get<CXXDefaultInitExpr>() ||
                ParentNode.get<CXXDefaultArgExpr>()) {
                return true; // Used as a default initializer
            }

            if (const auto* ParentDecl = ParentNode.get<Decl>()) {
                if (clang::isa<clang::ConceptDecl>(ParentDecl) ||
                    clang::isa<clang::StaticAssertDecl>(ParentDecl) ||
                    clang::isa<clang::NonTypeTemplateParmDecl>(ParentDecl)) {
                    return true;
                }
            }

            if (ParentNode.get<InitListExpr>() ||
                ParentNode.get<ExprWithCleanups>() ||
                ParentNode.get<CXXBindTemporaryExpr>()) {
                if (const auto* ParentStmt = ParentNode.get<Stmt>()) {
                    DeclParents = Result.Context->getParents(*ParentStmt);
                    continue;
                }
            }

            if (const auto* ParentStmt = ParentNode.get<Stmt>()) {
                DeclParents = Result.Context->getParents(*ParentStmt);
            } else if (const auto* ParentDecl = ParentNode.get<Decl>()) {
                DeclParents = Result.Context->getParents(*ParentDecl);
            } else {
                break;
            }
        }
    }

    const Stmt* Current = Parents.begin()->get<Stmt>();

    while (Current) {
        if (StopsAtBlock(dyn_cast<CompoundStmt>(Current))) break;
        if (clang::isa<UnaryExprOrTypeTraitExpr>(Current) ||
            clang::isa<CXXNoexceptExpr>(Current) ||
            clang::isa<CXXTypeidExpr>(Current)) {
            return true;
        }

        if (clang::isa<clang::RequiresExpr>(Current)) {
            return true;
        }


        if (isDiscardedCorrectly(Current)) return true;
        if (const auto* Construct = dyn_cast<CXXConstructExpr>(Current)) {
            for (const Expr* Arg : Construct->arguments())
                if (containsExpression(Arg, CallExpr)) return true;
        }
        if (const auto* BinaryOp = clang::dyn_cast<BinaryOperator>(Current)) {
            if (containsExpression(BinaryOp->getLHS(), CallExpr) ||
                containsExpression(BinaryOp->getRHS(), CallExpr)) {
                if (BinaryOp->getOpcode() == BO_Comma) {
                    if (containsExpression(BinaryOp->getLHS(), CallExpr)) return false;
                    // The RHS supplies the comma expression's result. Whether
                    // that result is used depends on the surrounding context.
                } else {
                    return true;
                }
            }
        } else if (const auto* UnaryOp = clang::dyn_cast<UnaryOperator>(Current)) {
            if (containsExpression(UnaryOp->getSubExpr(), CallExpr)) {
                return true;
            }
        } else if (const auto* Conditional = clang::dyn_cast<ConditionalOperator>(Current)) {
            if (containsExpression(Conditional->getCond(), CallExpr) ||
                containsExpression(Conditional->getTrueExpr(), CallExpr) ||
                containsExpression(Conditional->getFalseExpr(), CallExpr)) {
                return true;
            }
        } else if (const auto* Call = clang::dyn_cast<clang::CallExpr>(Current)) {
            for (const auto* Arg : Call->arguments()) {
                if (containsExpression(Arg, CallExpr)) {
                    return true;
                }
            }
        } else if (const auto* DS = clang::dyn_cast<DeclStmt>(Current)) {
            for (const auto* Decl : DS->decls()) {
                if (const auto* VD = clang::dyn_cast<VarDecl>(Decl)) {
                    const Expr* Init = VD->getInit();
                    if (Init && containsExpression(Init, CallExpr)) {
                        return true;
                    }
                }
            }
        } else if (const auto* RS = clang::dyn_cast<ReturnStmt>(Current)) {
            if (RS->getRetValue() && containsExpression(RS->getRetValue(), CallExpr)) {
                return true;
            }
        } else if (clang::isa<IfStmt>(Current) || clang::isa<WhileStmt>(Current) ||
                   clang::isa<DoStmt>(Current) || clang::isa<ForStmt>(Current) ||
                   clang::isa<SwitchStmt>(Current)) {
            const Stmt* Condition = nullptr;
            if (const auto* If = clang::dyn_cast<IfStmt>(Current)) {
                Condition = If->getCond();
            } else if (const auto* While = clang::dyn_cast<WhileStmt>(Current)) {
                Condition = While->getCond();
            } else if (const auto* Do = clang::dyn_cast<DoStmt>(Current)) {
                Condition = Do->getCond();
            } else if (const auto* For = clang::dyn_cast<ForStmt>(Current)) {
                Condition = For->getCond();
            } else if (const auto* Switch = clang::dyn_cast<SwitchStmt>(Current)) {
                Condition = Switch->getCond();
            }

            if (Condition && containsExpression(Condition, CallExpr)) {
                return true;
            }
        }

        auto NextParents = Result.Context->getParents(*Current);
        if (NextParents.empty()) {
            break;
        }

        const Stmt* ParentStmt = NextParents.begin()->get<Stmt>();
        if (!ParentStmt) {
            break;
        }

        Current = ParentStmt;
    }

    return false;
}

bool SdcReturnValueUsedCheck::containsExpression(const clang::Stmt* Container, const clang::Expr* Target) {
    if (!Container || !Target) {
        return false;
    }
    if (Container == Target) {
        return true;
    }
    for (const clang::Stmt* Child : Container->children()) {
        if (Child && containsExpression(Child, Target)) {
            return true;
        }
    }
    return false;
}

bool SdcReturnValueUsedCheck::isDiscardedCorrectly(const Stmt* Parent) {
    if (!Parent) {
        return false;
    }
    if (const auto* Cast = dyn_cast<ExplicitCastExpr>(Parent)) {
        return Cast->getTypeAsWritten()->isVoidType();
    }

    return false;
}

const Stmt* SdcReturnValueUsedCheck::getParentIgnoreParensAndCasts(
    const Stmt* S, ASTContext* Context) {
    if (!S) {
        return nullptr;
    }

    auto Parents = Context->getParents(*S);
    const Stmt* Parent = Parents.size() > 0 ? Parents.begin()->get<Stmt>() : nullptr;

    while (Parent) {
        if (!clang::isa<ParenExpr>(Parent) && !clang::isa<CastExpr>(Parent)) {
            break;
        }
        Parents = Context->getParents(*Parent);
        Parent = Parents.size() > 0 ? Parents.begin()->get<Stmt>() : nullptr;
    }

    return Parent;
}

} // namespace sdc
} // namespace tidy
} // namespace clang

