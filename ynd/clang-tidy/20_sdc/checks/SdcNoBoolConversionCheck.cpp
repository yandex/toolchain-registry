#include "SdcNoBoolConversionCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                // The cast operand reduced to an underlying expression that
                // still carries useful structure (lvalue MemberExpr, etc.). We
                // strip away LValueToRValue casts so we can inspect the
                // referenced FieldDecl when the operand is a bit-field.
                const Expr* peel(const Expr* E) {
                    while (E) {
                        if (const auto* IC = dyn_cast<ImplicitCastExpr>(E)) {
                            E = IC->getSubExpr();
                            continue;
                        }
                        if (const auto* P = dyn_cast<ParenExpr>(E)) {
                            E = P->getSubExpr();
                            continue;
                        }
                        break;
                    }
                    return E;
                }

                bool isOneBitBitField(const Expr* E, ASTContext& Ctx) {
                    const Expr* Inner = peel(E);
                    const auto* ME = dyn_cast_or_null<MemberExpr>(Inner);
                    if (!ME) return false;
                    const auto* FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
                    if (!FD || !FD->isBitField()) return false;
                    (void)Ctx;
                    return FD->getBitWidthValue() == 1;
                }

                // The cast appears as the condition / operand of a context
                // that performs a contextual conversion to bool (if / while /
                // for / do-while condition; ?:'s condition; operand of &&,
                // ||, !).
                bool inContextualBoolContext(const CastExpr* CE,
                                             ASTContext& Ctx) {
                    DynTypedNode N = DynTypedNode::create(*CE);
                    auto Parents = Ctx.getParents(N);
                    if (Parents.empty()) return false;
                    const DynTypedNode& P = Parents[0];
                    if (const auto* If = P.get<IfStmt>())
                        return If->getCond() == CE;
                    if (const auto* W = P.get<WhileStmt>())
                        return W->getCond() == CE;
                    if (const auto* D = P.get<DoStmt>())
                        return D->getCond() == CE;
                    if (const auto* F = P.get<ForStmt>())
                        return F->getCond() == CE;
                    if (const auto* CO = P.get<ConditionalOperator>())
                        return CO->getCond() == CE;
                    if (const auto* BO = P.get<BinaryOperator>())
                        return BO->isLogicalOp();
                    if (const auto* UO = P.get<UnaryOperator>())
                        return UO->getOpcode() == UO_LNot;
                    return false;
                }

                // The cast is the cond of a while whose condition is a
                // declaration (`while (T v = init)`) AND the converted
                // expression refers to that declared variable.
                bool isWhileCondVarConversion(const CastExpr* CE,
                                              ASTContext& Ctx) {
                    DynTypedNode N = DynTypedNode::create(*CE);
                    auto Parents = Ctx.getParents(N);
                    if (Parents.empty()) return false;
                    const auto* W = Parents[0].get<WhileStmt>();
                    if (!W || W->getCond() != CE) return false;
                    const VarDecl* CondVar = W->getConditionVariable();
                    if (!CondVar) return false;
                    const Expr* Inner = peel(CE->getSubExpr());
                    const auto* DRE = dyn_cast_or_null<DeclRefExpr>(Inner);
                    return DRE && DRE->getDecl() == CondVar;
                }

                // Walks across surrounding implicit casts and full-expressions
                // to find a BinaryOperator `=` (or `op=`) that has the cast as
                // its RHS. Returns nullptr if not the RHS of an assignment.
                const BinaryOperator* enclosingAssignment(const CastExpr* CE,
                                                          ASTContext& Ctx) {
                    DynTypedNode N = DynTypedNode::create(*CE);
                    for (int Depth = 0; Depth < 8; ++Depth) {
                        auto Parents = Ctx.getParents(N);
                        if (Parents.empty()) return nullptr;
                        N = Parents[0];
                        if (const auto* BO = N.get<BinaryOperator>()) {
                            if (BO->isAssignmentOp()) return BO;
                            return nullptr;
                        }
                        // Skip surrounding casts / parens; anything else
                        // breaks the chain.
                        if (!N.get<ImplicitCastExpr>() &&
                            !N.get<ParenExpr>() &&
                            !N.get<FullExpr>()) {
                            return nullptr;
                        }
                    }
                    return nullptr;
                }

                bool assignTargetIsOneBitBitField(const BinaryOperator* BO,
                                                  ASTContext& Ctx) {
                    return BO && isOneBitBitField(BO->getLHS(), Ctx);
                }

                bool isPointerType(QualType T) {
                    return T->isPointerType() || T->isMemberPointerType();
                }

                // True if RD or one of its bases declares an explicit
                // `operator bool()`.
                bool hasExplicitOperatorBool(const CXXRecordDecl* RD) {
                    if (!RD) return false;
                    RD = RD->getDefinition();
                    if (!RD) return false;
                    for (auto It = RD->conversion_begin(),
                              End = RD->conversion_end();
                         It != End; ++It) {
                        const auto* CD =
                            dyn_cast<CXXConversionDecl>(It.getDecl());
                        if (CD &&
                            CD->getConversionType()->isBooleanType() &&
                            CD->isExplicit()) {
                            return true;
                        }
                    }
                    // Inherited explicit operator bool.
                    for (const auto& Base : RD->bases()) {
                        if (const auto* BR =
                                Base.getType()->getAsCXXRecordDecl()) {
                            if (hasExplicitOperatorBool(BR)) return true;
                        }
                    }
                    return false;
                }

            } // namespace

            SdcNoBoolConversionCheck::SdcNoBoolConversionCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoBoolConversionCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    castExpr(unless(isExpansionInSystemHeader())).bind("cast"),
                    this);
            }

            void SdcNoBoolConversionCheck::check(
                const MatchFinder::MatchResult& Result) {
                const auto* CE = Result.Nodes.getNodeAs<CastExpr>("cast");
                if (!CE) return;
                ASTContext& Ctx = *Result.Context;

                QualType DstTy = CE->getType()
                                     .getCanonicalType()
                                     .getUnqualifiedType();

                // User-defined class -> bool conversions are represented as
                // a CastExpr whose subexpression is already a
                // CXXMemberCallExpr returning bool.  If we only compare the
                // cast's source/destination types below, both sides appear to
                // be bool and implicit operator bool() is missed.  Inspect the
                // conversion function before the same-type fast path.
                if (DstTy->isBooleanType() &&
                    CE->getCastKind() == CK_UserDefinedConversion) {
                    const auto* Call = dyn_cast<CXXMemberCallExpr>(
                        CE->getSubExpr()->IgnoreParenImpCasts());
                    const auto* Conv = Call
                        ? dyn_cast_or_null<CXXConversionDecl>(Call->getMethodDecl())
                        : nullptr;
                    if (Conv && Conv->getConversionType()->isBooleanType()) {
                        if (Conv->isExplicit()) {
                            return; // explicit operator bool is permitted
                        }

                        QualType ObjTy;
                        if (const Expr* Obj = Call->getImplicitObjectArgument()) {
                            ObjTy = Obj->getType();
                        }
                        diag(CE->getExprLoc(),
                             "conversion from %0 to 'bool' using non-explicit "
                             "operator bool is not allowed")
                            << (ObjTy.isNull() ? Conv->getThisType() : ObjTy);
                        return;
                    }
                }

                QualType SrcTy = CE->getSubExpr()
                                     ->getType()
                                     .getCanonicalType()
                                     .getUnqualifiedType();
                if (SrcTy == DstTy) return;

                // Skip casts in dependent (uninstantiated template) contexts.
                // The actual type may be valid once the template is instantiated,
                // and flagging at the template definition produces false positives
                // like "conversion from '<dependent type>' to 'bool'".
                if (SrcTy->isDependentType() || DstTy->isDependentType())
                    return;

                const bool SrcBool = SrcTy->isBooleanType();
                const bool DstBool = DstTy->isBooleanType();
                if (SrcBool == DstBool) return;

                // ----- Direction A: bool -> non-bool -----
                if (SrcBool) {
                    // Discard-via-(void) is a no-op pattern, not a conversion.
                    if (DstTy->isVoidType()) return;
                    if (CE->getCastKind() == CK_ConstructorConversion) {
                        return; // explicit conversion to class with bool ctor
                    }
                    if (const BinaryOperator* BO =
                            enclosingAssignment(CE, Ctx)) {
                        if (assignTargetIsOneBitBitField(BO, Ctx)) {
                            return; // assignment to 1-bit bit-field
                        }
                    }
                    // `==` / `!=` between two operands originally typed bool:
                    // clang promotes each to int for the comparison; the
                    // promotion is exempt by the rule.
                    DynTypedNode N = DynTypedNode::create(*CE);
                    auto Parents = Ctx.getParents(N);
                    if (!Parents.empty()) {
                        if (const auto* BO =
                                Parents[0].get<BinaryOperator>()) {
                            BinaryOperatorKind Op = BO->getOpcode();
                            if (Op == BO_EQ || Op == BO_NE) {
                                QualType L = BO->getLHS()
                                                ->IgnoreImpCasts()
                                                ->getType()
                                                .getCanonicalType();
                                QualType R = BO->getRHS()
                                                ->IgnoreImpCasts()
                                                ->getType()
                                                .getCanonicalType();
                                if (L->isBooleanType() &&
                                    R->isBooleanType()) {
                                    return;
                                }
                            }
                        }
                    }
                    diag(CE->getExprLoc(),
                         "conversion from 'bool' to %0 is not allowed")
                        << CE->getType();
                    return;
                }

                // ----- Direction B: non-bool -> bool -----
                // 1-bit bit-field source -> bool.
                if (isOneBitBitField(CE->getSubExpr(), Ctx)) {
                    return;
                }

                const bool Contextual = inContextualBoolContext(CE, Ctx);

                // Pointer in contextual context.
                if (Contextual && isPointerType(SrcTy)) {
                    return;
                }

                // `while (T v = ...)` declarator.
                if (isWhileCondVarConversion(CE, Ctx)) {
                    return;
                }

                // Class with explicit operator bool: the conversion is
                // performed by a CXXMemberCallExpr returning bool, so the
                // cast we see has type bool both above and below and is
                // already skipped by the SrcTy == DstTy guard. We add a
                // belt-and-braces guard here for code patterns where the
                // class type still appears as the cast source (e.g. trivial
                // wrappers); the AST shape may vary across clang versions.
                if (const auto* RD = SrcTy->getAsCXXRecordDecl()) {
                    if (hasExplicitOperatorBool(RD) && Contextual) {
                        return;
                    }
                }

                diag(CE->getExprLoc(),
                     "conversion from %0 to 'bool' is not allowed")
                    << CE->getSubExpr()->getType();
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
