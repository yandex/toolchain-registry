#include "SdcGetenvPointerConstQualifiedCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/Type.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/Analysis/CFG.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcGetenvPointerConstQualifiedCheck::SdcGetenvPointerConstQualifiedCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }
            void SdcGetenvPointerConstQualifiedCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    callExpr(callee(functionDecl(hasAnyName(
                                 "getenv", "localeconv", "setlocale", "strerror"))))
                        .bind("sensitive_call"),
                    this);

                // 2. Match variable declarations that contain these function calls anywhere
                // (This elegantly handles implicit casts, explicit casts, and temporaries)
                Finder->addMatcher(
                    declStmt(
                        hasDescendant(
                            callExpr(
                                callee(functionDecl(hasAnyName(
                                    "getenv", "localeconv", "setlocale", "strerror"
                                )))
                            )
                        )
                    ).bind("decl_with_call"),
                    this);

                // 3. Match member access expressions (for lconv struct fields)
                Finder->addMatcher(
                    memberExpr().bind("member_access"),
                    this);
            }

            void SdcGetenvPointerConstQualifiedCheck::check(
                const MatchFinder::MatchResult& Result) {
                // Check for direct calls to sensitive functions
                if (const auto* Call = Result.Nodes.getNodeAs<CallExpr>("sensitive_call")) {
                    checkFunctionCall(Call, Result);
                }

                // Check for variable declarations that use sensitive function results
                if (const auto* DeclStmt = Result.Nodes.getNodeAs<clang::DeclStmt>("decl_with_call")) {
                    checkVariableDeclaration(DeclStmt, Result);
                }

                // Check for member access expressions (for lconv struct fields)
                if (const auto* MemberExpr = Result.Nodes.getNodeAs<clang::MemberExpr>("member_access")) {
                    checkLconvMemberModification(MemberExpr, Result);
                }
            }

            void SdcGetenvPointerConstQualifiedCheck::checkFunctionCall(
                const CallExpr* Call, const MatchFinder::MatchResult& Result) {
                // Get the parent statement/context to see how the result is used
                const clang::Stmt* Parent = getParentIgnoreParensAndCasts(Call, Result.Context);

                if (!Parent) {
                    return;
                }

                // Check various usage patterns that violate the rule
                checkForModificationUsage(Call, Parent, Result);

                // Check for function call parameter usage
                checkFunctionParameterUsage(Call, Parent, Result);
            }

            void SdcGetenvPointerConstQualifiedCheck::checkVariableDeclaration(
                const clang::DeclStmt* DeclStmt, const MatchFinder::MatchResult& Result) {
                for (const auto* Decl : DeclStmt->decls()) {
                    if (const auto* VarDecl = llvm::dyn_cast<clang::VarDecl>(Decl)) {
                        checkVariableTypeAndUsage(VarDecl, Result);
                    }
                }
            }
            void SdcGetenvPointerConstQualifiedCheck::checkVariableTypeAndUsage(
                const clang::VarDecl* VarDecl, const MatchFinder::MatchResult& Result) {

                const clang::CallExpr* SensitiveCall = getSensitiveOrigin(VarDecl->getInit());
                if (!SensitiveCall) return;

                clang::QualType TypeToCheck = VarDecl->getType();

                if (TypeToCheck->isReferenceType()) {
                    TypeToCheck = TypeToCheck->getPointeeType();
                }

                // FIX: Unwrap arrays! If it's char* arr[], we want to check char*
                if (TypeToCheck->isArrayType()) {
                    TypeToCheck = VarDecl->getASTContext().getBaseElementType(TypeToCheck);
                }

                if (TypeToCheck->isPointerType()) {
                    clang::QualType PointeeType = TypeToCheck->getPointeeType();
                    if (!PointeeType.isConstQualified()) {
                        diag(VarDecl->getLocation(),
                             "pointer returned by '%0' must be treated as pointer to const-qualified type. "
                             "Note: arrays, auto&&, and explicit mutable casts bypass constness.")
                            << getFunctionNameFromCall(SensitiveCall);
                    }
                }
            }

            const clang::CallExpr* SdcGetenvPointerConstQualifiedCheck::getSensitiveOrigin(
                const clang::Expr* E) {
                if (!E) return nullptr;

                E = E->IgnoreParenCasts();

                while (true) {
                    if (const auto* EWC = llvm::dyn_cast<clang::ExprWithCleanups>(E)) {
                        E = EWC->getSubExpr()->IgnoreParenCasts();
                    } else if (const auto* MTE = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(E)) {
                        E = MTE->getSubExpr()->IgnoreParenCasts();
                    } else if (const auto* BTE = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(E)) {
                        E = BTE->getSubExpr()->IgnoreParenCasts();
                    } else if (const auto* CondOp = llvm::dyn_cast<clang::ConditionalOperator>(E)) {
                        // FIX: It's a ternary operator. Check both branches!
                        if (const auto* TrueBranch = getSensitiveOrigin(CondOp->getTrueExpr()))
                            return TrueBranch;
                        if (const auto* FalseBranch = getSensitiveOrigin(CondOp->getFalseExpr()))
                            return FalseBranch;
                        return nullptr;
                     } else if (const auto* InitList = llvm::dyn_cast<clang::InitListExpr>(E)) {
                        for (unsigned I = 0; I < InitList->getNumInits(); ++I) {
                            if (const auto* Origin = getSensitiveOrigin(InitList->getInit(I)))
                                return Origin;
                        }
                        return nullptr;
                    } else {
                        break;
                    }
                }

                if (const auto* Call = llvm::dyn_cast<clang::CallExpr>(E)) {
                    if (const clang::FunctionDecl* Func = Call->getDirectCallee()) {
                        llvm::StringRef Name = Func->getName();
                        if (Name == "getenv" || Name == "localeconv" ||
                            Name == "setlocale" || Name == "strerror") {
                            return Call;
                        }
                    }
                }
                return nullptr;
            }

            void SdcGetenvPointerConstQualifiedCheck::checkForModificationUsage(
                const clang::Stmt* SensitiveStmt, const clang::Stmt* Parent,
                const MatchFinder::MatchResult& Result) {
                // Check if the sensitive result is being modified
                if (const auto* UnaryOp = llvm::dyn_cast<clang::UnaryOperator>(Parent)) {
                    if (UnaryOp->getOpcode() == clang::UO_Deref &&
                        UnaryOp->getSubExpr()->IgnoreParenImpCasts() == SensitiveStmt) {
                        // Dereferencing the pointer - check if it's used in a modification context
                        checkDereferenceModification(UnaryOp, Result);
                    }
                }

                if (const auto* ArraySubscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(Parent)) {
                    if (ArraySubscript->getBase()->IgnoreParenImpCasts() == SensitiveStmt) {
                        // Array subscript operation - check if it's being modified
                        checkArraySubscriptModification(ArraySubscript, Result);
                    }
                }

                if (const auto* MemberExpr = llvm::dyn_cast<clang::MemberExpr>(Parent)) {
                    if (MemberExpr->getBase()->IgnoreParenImpCasts() == SensitiveStmt) {
                        // Structure member access - check if it's being modified
                        checkMemberAccessModification(MemberExpr, Result);
                    }
                }

                if (const auto* BinaryOp = llvm::dyn_cast<clang::BinaryOperator>(Parent)) {
                    if (BinaryOp->getLHS()->IgnoreParenImpCasts() == SensitiveStmt ||
                        (BinaryOp->getLHS()->IgnoreParenImpCasts() != SensitiveStmt &&
                         hasSensitiveStmtInSubtree(BinaryOp->getLHS(), SensitiveStmt))) {
                        // Assignment or other operation involving the sensitive pointer
                        checkBinaryOperationModification(BinaryOp, Result);
                    }
                }
            }

            void SdcGetenvPointerConstQualifiedCheck::checkFunctionParameterUsage(
                const clang::CallExpr* SensitiveCall, const clang::Stmt* Parent,
                const MatchFinder::MatchResult& Result) {

                const clang::Stmt* CurrentParent = Parent;

                // Walk up the tree if the parent is a Ternary Operator (?:)
                while (CurrentParent && llvm::isa<clang::ConditionalOperator>(CurrentParent)) {
                    CurrentParent = getParentIgnoreParensAndCasts(CurrentParent, Result.Context);
                }

                // Now safely check if the resolved parent is a function call
                if (const auto* OuterCall = llvm::dyn_cast_or_null<clang::CallExpr>(CurrentParent)) {
                    checkCallParameterConstQualification(SensitiveCall, OuterCall, Result);
                }
            }

            void SdcGetenvPointerConstQualifiedCheck::checkCallParameterConstQualification(
                const clang::CallExpr* SensitiveCall, const clang::CallExpr* OuterCall,
                const MatchFinder::MatchResult& Result) {

                int ParamIndex = -1;
                unsigned ArgOffset = llvm::isa<clang::CXXOperatorCallExpr>(OuterCall) ? 1 : 0;

                for (unsigned I = ArgOffset; I < OuterCall->getNumArgs(); ++I) {
                    const clang::Expr* Arg = OuterCall->getArg(I);
                    if (!Arg) continue;

                    Arg = Arg->IgnoreParenImpCasts();

                    // Safely strip temporaries. If Arg ever becomes null, the loop gracefully exits.
                    while (Arg) {
                        if (const auto* EWC = clang::dyn_cast<clang::ExprWithCleanups>(Arg)) {
                            Arg = EWC->getSubExpr()->IgnoreParenImpCasts();
                        } else if (const auto* MTE = clang::dyn_cast<clang::MaterializeTemporaryExpr>(Arg)) {
                            Arg = MTE->getSubExpr()->IgnoreParenImpCasts();
                        } else if (const auto* BTE = clang::dyn_cast<clang::CXXBindTemporaryExpr>(Arg)) {
                            Arg = BTE->getSubExpr()->IgnoreParenImpCasts();
                        } else {
                            break;
                        }
                    }

                    // Safely check the subtree now that we guarantee Arg is not null!
                    if (Arg && (Arg == SensitiveCall || hasSensitiveStmtInSubtree(Arg, SensitiveCall))) {
                        ParamIndex = I - ArgOffset;
                        break;
                    }
                }

                if (ParamIndex == -1) return;

                // For lambdas, OuterCall->getDirectCallee() can be null. We must cast to CXXOperatorCallExpr
                const clang::FunctionDecl* FuncDecl = OuterCall->getDirectCallee();
                if (!FuncDecl) {
                    if (const auto* OpCall = llvm::dyn_cast<clang::CXXOperatorCallExpr>(OuterCall)) {
                        FuncDecl = OpCall->getDirectCallee();
                    }
                }

                if (!FuncDecl) return;

                if (ParamIndex < (int)FuncDecl->getNumParams()) {
                    const clang::ParmVarDecl* Param = FuncDecl->getParamDecl(ParamIndex);
                    clang::QualType ParamType = Param->getType();

                    clang::QualType TypeToCheck = ParamType;

                    // Unwrap reference (handles auto&& / T&&)
                    if (TypeToCheck->isReferenceType()) {
                        TypeToCheck = TypeToCheck->getPointeeType();
                    }

                    // Check the pointee's constness
                    if (TypeToCheck->isPointerType()) {
                        clang::QualType PointeeType = TypeToCheck->getPointeeType();

                        if (!PointeeType.isConstQualified()) {
                            diag(SensitiveCall->getBeginLoc(),
                                 "pointer returned by '%0' must be passed to a context expecting a pointer to a const-qualified type. "
                                 "Warning: Passing to forwarding references (auto&&) allows mutable access.")
                                << getFunctionNameFromCall(SensitiveCall)
                                << getParameterLocationNote(Param, ParamIndex);
                        }
                    }
                }
            }

            void SdcGetenvPointerConstQualifiedCheck::checkDereferenceModification(
                const clang::UnaryOperator* UnaryOp, const MatchFinder::MatchResult& Result) {
                // Check if this dereference is used in a modification context
                const clang::Stmt* Parent = getParentIgnoreParensAndCasts(UnaryOp, Result.Context);

                if (const auto* Assignment = llvm::dyn_cast<clang::BinaryOperator>(Parent)) {
                    if (Assignment->isAssignmentOp() &&
                        Assignment->getLHS()->IgnoreParenImpCasts() == UnaryOp) {
                        // Direct assignment to dereferenced pointer - violation
                        diag(UnaryOp->getOperatorLoc(),
                             "modification of data through pointer returned by sensitive function");
                    }
                }

                // Check for increment/decrement operations on the dereferenced value
                if (const auto* IncDecOp = llvm::dyn_cast<clang::UnaryOperator>(Parent)) {
                    if (IncDecOp->isIncrementDecrementOp() &&
                        IncDecOp->getSubExpr()->IgnoreParenImpCasts() == UnaryOp) {
                        diag(UnaryOp->getOperatorLoc(),
                             "modification of data through pointer returned by sensitive function");
                    }
                }
            }

            void SdcGetenvPointerConstQualifiedCheck::checkArraySubscriptModification(
                const clang::ArraySubscriptExpr* ArraySubscript, const MatchFinder::MatchResult& Result) {
                const clang::Stmt* Parent = getParentIgnoreParensAndCasts(ArraySubscript, Result.Context);

                if (const auto* Assignment = llvm::dyn_cast<clang::BinaryOperator>(Parent)) {
                    if (Assignment->isAssignmentOp() &&
                        Assignment->getLHS()->IgnoreParenImpCasts() == ArraySubscript) {
                        diag(ArraySubscript->getExprLoc(),
                             "modification of array element through pointer returned by sensitive function");
                    }
                }
            }

            void SdcGetenvPointerConstQualifiedCheck::checkMemberAccessModification(
                const clang::MemberExpr* MemberExpr, const MatchFinder::MatchResult& Result) {
                const clang::Stmt* Parent = getParentIgnoreParensAndCasts(MemberExpr, Result.Context);

                if (const auto* Assignment = llvm::dyn_cast<clang::BinaryOperator>(Parent)) {
                    if (Assignment->isAssignmentOp() &&
                        Assignment->getLHS()->IgnoreParenImpCasts() == MemberExpr) {
                        diag(MemberExpr->getMemberLoc(),
                             "modification of structure member through pointer returned by sensitive function");
                    }
                }
            }

            void SdcGetenvPointerConstQualifiedCheck::checkBinaryOperationModification(
                const clang::BinaryOperator* BinaryOp, const MatchFinder::MatchResult& Result) {
                if (BinaryOp->isAssignmentOp()) {
                    // Assignment to the pointer itself
                    diag(BinaryOp->getOperatorLoc(),
                         "assignment to pointer returned by sensitive function");
                }

                // Check for pointer arithmetic that could be used for modification
                if (BinaryOp->isAdditiveOp()) {
                    diag(BinaryOp->getOperatorLoc(),
                         "pointer arithmetic on result of sensitive function");
                }
            }
            const clang::Stmt* SdcGetenvPointerConstQualifiedCheck::getParentIgnoreParensAndCasts(
                const clang::Stmt* S, ASTContext* Context) {
                if (!S) {
                    return nullptr;
                }

                auto Parents = Context->getParents(*S);
                if (Parents.empty()) {
                    return nullptr;
                }

                const clang::Stmt* Parent = Parents.begin()->get<clang::Stmt>();

                // FIX: Skip parens, casts, AND temporary materializations
                while (Parent && (
                    llvm::isa<clang::ParenExpr>(Parent) ||
                    llvm::isa<clang::CastExpr>(Parent) ||
                    llvm::isa<clang::MaterializeTemporaryExpr>(Parent) ||
                    llvm::isa<clang::CXXBindTemporaryExpr>(Parent) ||
                    llvm::isa<clang::ExprWithCleanups>(Parent))) {

                    Parents = Context->getParents(*Parent);
                    if (Parents.empty()) {
                        break;
                    }
                    Parent = Parents.begin()->get<clang::Stmt>();
                }

                return Parent;
            }

            bool SdcGetenvPointerConstQualifiedCheck::hasSensitiveStmtInSubtree(
                const clang::Stmt* Root, const clang::Stmt* Target) {

                if (!Root || !Target) {
                    return false;
                }

                if (Root == Target) {
                    return true;
                }

                for (const auto& Child : Root->children()) {
                    if (Child && hasSensitiveStmtInSubtree(Child, Target)) {
                        return true;
                    }
                }

                return false;
            }

            StringRef SdcGetenvPointerConstQualifiedCheck::getFunctionNameFromInitializer(
                const clang::Expr* Init) {
                if (!Init) {
                    return "unknown";
                }

                const CallExpr* Call = llvm::dyn_cast<CallExpr>(Init->IgnoreParenImpCasts());
                if (!Call) {
                    return "unknown";
                }

                const clang::FunctionDecl* Func = Call->getDirectCallee();
                if (!Func) {
                    return "unknown";
                }

                return Func->getName();
            }

            StringRef SdcGetenvPointerConstQualifiedCheck::getFunctionNameFromCall(
                const clang::CallExpr* Call) {
                if (!Call) {
                    return "unknown";
                }

                const clang::FunctionDecl* Func = Call->getDirectCallee();
                if (!Func) {
                    return "unknown";
                }

                return Func->getName();
            }

            clang::FixItHint SdcGetenvPointerConstQualifiedCheck::getParameterLocationNote(
                const clang::ParmVarDecl* Param, int ParamIndex) {
                // Create a simple note pointing to the function parameter declaration
                return clang::FixItHint::CreateInsertion(
                    Param->getLocation(),
                    " (function parameter)");
            }

            bool SdcGetenvPointerConstQualifiedCheck::isLconvPointerField(
                const clang::FieldDecl* Field) {
                if (!Field) {
                    return false;
                }

                // Check if the field is in the lconv struct
                const clang::RecordDecl* ParentRecord = Field->getParent();
                if (!ParentRecord || ParentRecord->getName() != "lconv") {
                    return false;
                }

                // Check if the field is a char pointer (all string fields in lconv are char*)
                clang::QualType FieldType = Field->getType();
                if (!FieldType->isPointerType()) {
                    return false;
                }

                clang::QualType PointeeType = FieldType->getPointeeType();
                return PointeeType->isCharType();
            }

            bool SdcGetenvPointerConstQualifiedCheck::isFromLocaleconvCall(
                const clang::Expr* E, const MatchFinder::MatchResult& Result) {
                if (!E) {
                    return false;
                }

                E = E->IgnoreParenImpCasts();

                // Direct call to localeconv()
                if (const auto* Call = llvm::dyn_cast<clang::CallExpr>(E)) {
                    if (const clang::FunctionDecl* Func = Call->getDirectCallee()) {
                        if (Func->getName() == "localeconv") {
                            return true;
                        }
                    }
                }

                // Variable reference - check if it was initialized from localeconv()
                if (const auto* DeclRef = llvm::dyn_cast<clang::DeclRefExpr>(E)) {
                    if (const auto* VarDecl = llvm::dyn_cast<clang::VarDecl>(DeclRef->getDecl())) {
                        if (const clang::Expr* Init = VarDecl->getInit()) {
                            return isFromLocaleconvCall(Init, Result);
                        }
                    }
                }

                return false;
            }

            const clang::Expr* SdcGetenvPointerConstQualifiedCheck::findSensitiveOrigin(
                const clang::Expr* E, const MatchFinder::MatchResult& Result) {
                if (!E) {
                    return nullptr;
                }

                E = E->IgnoreParenImpCasts();

                // Check if this is a call to a sensitive function
                if (const auto* Call = llvm::dyn_cast<clang::CallExpr>(E)) {
                    if (const clang::FunctionDecl* Func = Call->getDirectCallee()) {
                        llvm::StringRef FuncName = Func->getName();
                        if (FuncName == "getenv" || FuncName == "localeconv" ||
                            FuncName == "setlocale" || FuncName == "strerror") {
                            return Call;
                        }
                    }
                }

                // Check if this is a variable reference
                if (const auto* DeclRef = llvm::dyn_cast<clang::DeclRefExpr>(E)) {
                    if (const auto* VarDecl = llvm::dyn_cast<clang::VarDecl>(DeclRef->getDecl())) {
                        if (const clang::Expr* Init = VarDecl->getInit()) {
                            return findSensitiveOrigin(Init, Result);
                        }
                    }
                }

                return nullptr;
            }

            bool SdcGetenvPointerConstQualifiedCheck::isMemberExprFromLocaleconv(
                const clang::MemberExpr* MemberExpr, const MatchFinder::MatchResult& Result) {
                if (!MemberExpr) {
                    return false;
                }

                // Get the field being accessed
                const clang::ValueDecl* Member = MemberExpr->getMemberDecl();
                const auto* Field = llvm::dyn_cast<clang::FieldDecl>(Member);

                // Check if it's a pointer field in lconv struct
                if (!isLconvPointerField(Field)) {
                    return false;
                }

                // Check if the base expression comes from localeconv()
                const clang::Expr* Base = MemberExpr->getBase();
                return isFromLocaleconvCall(Base, Result);
            }
            void SdcGetenvPointerConstQualifiedCheck::checkLconvMemberModification(
                const clang::MemberExpr* MemberExpr, const MatchFinder::MatchResult& Result) {
                // Only proceed if this member access is from a localeconv() result
                if (!isMemberExprFromLocaleconv(MemberExpr, Result)) {
                    return;
                }

                // Get the parent to see how this member is being used
                // NOTE: Because we updated getParentIgnoreParensAndCasts earlier,
                // this will now successfully "see through" temporaries!
                const clang::Stmt* Parent = getParentIgnoreParensAndCasts(MemberExpr, Result.Context);
                if (!Parent) {
                    return;
                }

                const auto* Field = llvm::dyn_cast<clang::FieldDecl>(MemberExpr->getMemberDecl());
                llvm::StringRef FieldName = Field ? Field->getName() : "unknown field";

                // 1. Check for direct assignment to the member (unchanged)
                if (const auto* BinaryOp = llvm::dyn_cast_or_null<clang::BinaryOperator>(Parent)) {
                    if (BinaryOp->isAssignmentOp() &&
                        BinaryOp->getLHS()->IgnoreParenImpCasts() == MemberExpr) {
                        diag(MemberExpr->getMemberLoc(),
                             "modification of lconv pointer field '%0' returned by localeconv()")
                            << FieldName;
                        return;
                    }
                }

                // 2. Check for dereference and modification (unchanged)
                if (const auto* ArraySubscript = llvm::dyn_cast_or_null<clang::ArraySubscriptExpr>(Parent)) {
                    if (ArraySubscript->getBase()->IgnoreParenImpCasts() == MemberExpr) {
                        const clang::Stmt* GrandParent = getParentIgnoreParensAndCasts(ArraySubscript, Result.Context);
                        if (GrandParent) {
                            if (const auto* Assignment = llvm::dyn_cast<clang::BinaryOperator>(GrandParent)) {
                                if (Assignment->isAssignmentOp() &&
                                    Assignment->getLHS()->IgnoreParenImpCasts() == ArraySubscript) {
                                    diag(MemberExpr->getMemberLoc(),
                                         "modification of data through lconv pointer field '%0' returned by localeconv()")
                                        << FieldName;
                                    return;
                                }
                            }
                        }
                    }
                }

                // 3. FIX: Check for function parameter usage (Updated to handle lambdas and auto&&)
                if (const auto* CallExpr = llvm::dyn_cast_or_null<clang::CallExpr>(Parent)) {
                    int ParamIndex = -1;

                    // Handle lambda 'this' argument offset
                    unsigned ArgOffset = llvm::isa<clang::CXXOperatorCallExpr>(CallExpr) ? 1 : 0;

                    for (unsigned I = ArgOffset; I < CallExpr->getNumArgs(); ++I) {
                        const clang::Expr* Arg = CallExpr->getArg(I)->IgnoreParenImpCasts();

                        // Strip Temporaries
                        if (const auto* EWC = clang::dyn_cast<clang::ExprWithCleanups>(Arg)) {
                            Arg = EWC->getSubExpr()->IgnoreParenImpCasts();
                        }
                        if (const auto* MTE = clang::dyn_cast<clang::MaterializeTemporaryExpr>(Arg)) {
                            Arg = MTE->getSubExpr()->IgnoreParenImpCasts();
                        }
                        if (const auto* BTE = clang::dyn_cast<clang::CXXBindTemporaryExpr>(Arg)) {
                            Arg = BTE->getSubExpr()->IgnoreParenImpCasts();
                        }

                        if (Arg == MemberExpr) {
                            ParamIndex = I - ArgOffset; // Correct the index
                            break;
                        }
                    }

                    if (ParamIndex != -1) {
                        // Resolve lambda direct callee
                        const clang::FunctionDecl* FuncDecl = CallExpr->getDirectCallee();
                        if (!FuncDecl) {
                            if (const auto* OpCall = llvm::dyn_cast<clang::CXXOperatorCallExpr>(CallExpr)) {
                                FuncDecl = OpCall->getDirectCallee();
                            }
                        }

                        if (FuncDecl && ParamIndex < (int)FuncDecl->getNumParams()) {
                            const clang::ParmVarDecl* Param = FuncDecl->getParamDecl(ParamIndex);
                            clang::QualType ParamType = Param->getType();
                            clang::QualType TypeToCheck = ParamType;

                            // Unwrap references (handles auto&& / T&&)
                            if (TypeToCheck->isReferenceType()) {
                                TypeToCheck = TypeToCheck->getPointeeType();
                            }

                            // Check if the parameter expects a non-const pointer
                            if (TypeToCheck->isPointerType()) {
                                clang::QualType PointeeType = TypeToCheck->getPointeeType();
                                if (!PointeeType.isConstQualified()) {
                                    diag(MemberExpr->getMemberLoc(),
                                         "lconv pointer field '%0' from localeconv() passed to function expecting non-const pointer. "
                                         "Warning: Passing to forwarding references (auto&&) allows mutable access.")
                                        << FieldName;
                                }
                            }
                        }
                    }
                }
            }
        } // namespace sdc
    } // namespace tidy
} // namespace clang
