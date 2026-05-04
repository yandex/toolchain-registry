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

            void SdcGetenvPointerConstQualifiedCheck::registerMatchers(
                MatchFinder* Finder) {
                // Match calls to the sensitive functions
                Finder->addMatcher(
                    callExpr(callee(functionDecl(
                                 hasAnyName(
                                     "getenv",
                                     "localeconv",
                                     "setlocale",
                                     "strerror"))))
                        .bind("sensitive_call"),
                    this);

                // Match variable declarations that use these function results
                Finder->addMatcher(
                    declStmt(hasDescendant(
                                 varDecl(hasInitializer(callExpr(callee(functionDecl(hasAnyName(
                                     "getenv",
                                     "localeconv",
                                     "setlocale",
                                     "strerror"))))))))
                        .bind("decl_with_call"),
                    this);

                // Match member access expressions (for lconv struct fields)
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
                // Check if the variable type is properly const-qualified
                clang::QualType VarType = VarDecl->getType();

                // For pointer types, the pointee should be const
                if (VarType->isPointerType()) {
                    clang::QualType PointeeType = VarType->getPointeeType();
                    if (!PointeeType.isConstQualified()) {
                        // Variable declared without const qualification - this is a violation
                        diag(VarDecl->getLocation(),
                             "pointer returned by '%0' must be treated as pointer to const-qualified type")
                            << getFunctionNameFromInitializer(VarDecl->getInit());
                    }
                }

                // For now, we'll rely on the modification checking through call expressions
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
                // Check if the sensitive call result is used as a parameter in another function call
                if (const auto* OuterCall = llvm::dyn_cast<clang::CallExpr>(Parent)) {
                    checkCallParameterConstQualification(SensitiveCall, OuterCall, Result);
                }
            }

            void SdcGetenvPointerConstQualifiedCheck::checkCallParameterConstQualification(
                const clang::CallExpr* SensitiveCall, const clang::CallExpr* OuterCall,
                const MatchFinder::MatchResult& Result) {
                // Find which parameter position the sensitive call is in
                int ParamIndex = -1;
                for (unsigned I = 0; I < OuterCall->getNumArgs(); ++I) {
                    if (OuterCall->getArg(I)->IgnoreParenImpCasts() == SensitiveCall) {
                        ParamIndex = I;
                        break;
                    }
                }

                if (ParamIndex == -1) {
                    return;
                }

                // Get the called function declaration
                const clang::FunctionDecl* FuncDecl = OuterCall->getDirectCallee();
                if (!FuncDecl) {
                    // Can't determine parameter types for indirect calls
                    return;
                }

                // Check if the corresponding parameter is const-qualified
                if (ParamIndex < (int)FuncDecl->getNumParams()) {
                    const clang::ParmVarDecl* Param = FuncDecl->getParamDecl(ParamIndex);
                    clang::QualType ParamType = Param->getType();

                    // For pointer types, check if the pointee is const
                    if (ParamType->isPointerType()) {
                        clang::QualType PointeeType = ParamType->getPointeeType();
                        if (!PointeeType.isConstQualified()) {
                            // Parameter is not const-qualified - violation
                            diag(SensitiveCall->getBeginLoc(),
                                 "pointer returned by '%0' must be passed as pointer to const-qualified type")
                                << getFunctionNameFromCall(SensitiveCall)
                                << getParameterLocationNote(Param, ParamIndex);
                        }
                    } else if (ParamType->isReferenceType()) {
                        // For reference types, check if the referenced type is const
                        clang::QualType ReferencedType = ParamType->getPointeeType();
                        if (!ReferencedType.isConstQualified()) {
                            diag(SensitiveCall->getBeginLoc(),
                                 "pointer returned by '%0' must be passed as reference to const-qualified type")
                                << getFunctionNameFromCall(SensitiveCall)
                                << getParameterLocationNote(Param, ParamIndex);
                        }
                    }
                    // For non-pointer/non-reference types, rule doesn't apply
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

                // Skip parens and casts when looking for the actual parent
                while (Parent && (llvm::isa<clang::ParenExpr>(Parent) || llvm::isa<clang::CastExpr>(Parent))) {
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
                const clang::Stmt* Parent = getParentIgnoreParensAndCasts(MemberExpr, Result.Context);
                if (!Parent) {
                    return;
                }

                const auto* Field = llvm::dyn_cast<clang::FieldDecl>(MemberExpr->getMemberDecl());
                llvm::StringRef FieldName = Field ? Field->getName() : "unknown field";

                // Check for direct assignment to the member (pointer field itself)
                if (const auto* BinaryOp = llvm::dyn_cast_or_null<clang::BinaryOperator>(Parent)) {
                    if (BinaryOp->isAssignmentOp() &&
                        BinaryOp->getLHS()->IgnoreParenImpCasts() == MemberExpr) {
                        diag(MemberExpr->getMemberLoc(),
                             "modification of lconv pointer field '%0' returned by localeconv()")
                            << FieldName;
                        return;
                    }
                }

                // Check for dereference and modification of the data pointed to by the member
                if (const auto* ArraySubscript = llvm::dyn_cast_or_null<clang::ArraySubscriptExpr>(Parent)) {
                    if (ArraySubscript->getBase()->IgnoreParenImpCasts() == MemberExpr) {
                        // Check if the array element is being modified
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

                // Check for function parameter usage
                if (const auto* CallExpr = llvm::dyn_cast_or_null<clang::CallExpr>(Parent)) {
                    // Find which parameter position this member expression is in
                    int ParamIndex = -1;
                    for (unsigned I = 0; I < CallExpr->getNumArgs(); ++I) {
                        if (CallExpr->getArg(I)->IgnoreParenImpCasts() == MemberExpr) {
                            ParamIndex = I;
                            break;
                        }
                    }

                    if (ParamIndex != -1) {
                        const clang::FunctionDecl* FuncDecl = CallExpr->getDirectCallee();
                        if (FuncDecl && ParamIndex < (int)FuncDecl->getNumParams()) {
                            const clang::ParmVarDecl* Param = FuncDecl->getParamDecl(ParamIndex);
                            clang::QualType ParamType = Param->getType();

                            // Check if the parameter expects a non-const pointer
                            if (ParamType->isPointerType()) {
                                clang::QualType PointeeType = ParamType->getPointeeType();
                                if (!PointeeType.isConstQualified()) {
                                    diag(MemberExpr->getMemberLoc(),
                                         "lconv pointer field '%0' from localeconv() passed to function expecting non-const pointer")
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
