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
    : ClangTidyCheck(Name, Context)
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

    // Match ALL function calls inside EVERY constructor member initializer
    Finder->addMatcher(
        cxxConstructorDecl(
            unless(isExpansionInSystemHeader()),
            forEachConstructorInitializer(
                cxxCtorInitializer(
                    withInitializer(
                        findAll(callExpr().bind("ctor_init_call"))
                    )
                )
            )
        ),
        this);
}


void SdcReturnValueUsedCheck::check(const MatchFinder::MatchResult& Result) {
    // Check if this is a call in a constructor initializer (these are always "used")
    if (const auto* CtorInitCall = Result.Nodes.getNodeAs<CallExpr>("ctor_init_call")) {
        // Add this call to the set of constructor initializer calls
        CtorInitCalls.insert(CtorInitCall);
        return;
    }

    if (const auto* Call = Result.Nodes.getNodeAs<CallExpr>("call_expr")) {
        // Skip if this call is used in a constructor initializer
        if (CtorInitCalls.count(Call) > 0) {
            return;
        }
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
        diag(Call->getBeginLoc(),
             "return value of function call is not used; if intended to discard, cast to void");
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
                    diag(OperatorCall->getBeginLoc(),
                         "return value of lambda call is not used; if intended to discard, cast to void");
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
        diag(MemberCall->getBeginLoc(),
             "return value of member function call is not used; if intended to discard, cast to void");
    }
}

bool SdcReturnValueUsedCheck::isReturnValueUsed(
    const Expr* CallExpr, const MatchFinder::MatchResult& Result) {

    auto Parents = Result.Context->getParents(*CallExpr);
    if (Parents.empty()) {
        return false;
    }

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
        const auto* ParentStmt = UpwardParents.begin()->get<Stmt>();
        if (!ParentStmt) break;
        UpwardParents = Result.Context->getParents(*ParentStmt);
    }

    {
        auto DeclParents = Parents;
        while (!DeclParents.empty()) {
            const auto& ParentNode = *DeclParents.begin();

            if (const auto* VD = ParentNode.get<VarDecl>()) {
                return true;
            }
            if (const auto* FD = ParentNode.get<FieldDecl>()) {
                return true;
            }
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
        if (clang::isa<UnaryExprOrTypeTraitExpr>(Current) ||
            clang::isa<CXXNoexceptExpr>(Current) ||
            clang::isa<CXXTypeidExpr>(Current)) {
            return true;
        }

        if (clang::isa<clang::RequiresExpr>(Current)) {
            return true;
        }


        if (const auto* BinaryOp = clang::dyn_cast<BinaryOperator>(Current)) {
            if (containsExpression(BinaryOp->getLHS(), CallExpr) ||
                containsExpression(BinaryOp->getRHS(), CallExpr)) {
                if (BinaryOp->getOpcode() == BO_Comma &&
                    containsExpression(BinaryOp->getLHS(), CallExpr)) {
                    return false;
                }
                return true;
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
    if (const auto* CStyleCast = clang::dyn_cast<CStyleCastExpr>(Parent)) {
        if (CStyleCast->getTypeAsWritten()->isVoidType()) {
            return true;
        }
    }
    if (const auto* FunctionalCast = clang::dyn_cast<CXXFunctionalCastExpr>(Parent)) {
        if (FunctionalCast->getTypeAsWritten()->isVoidType()) {
            return true;
        }
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

