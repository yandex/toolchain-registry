#include "SdcBitwiseOperandsUnsignedCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/APSInt.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                // The operand's source-level type, before integral promotion
                // casts inserted by the compiler.
                QualType preCastType(const Expr* E) {
                    return E->IgnoreImpCasts()
                        ->getType()
                        .getCanonicalType()
                        .getUnqualifiedType();
                }

                bool isUnsigned(QualType T) {
                    // isUnsignedIntegerType covers bool, unsigned char/short/
                    // int/long/long long and enums with unsigned underlying
                    // type, which is the behaviour the rule wants.
                    return T->isUnsignedIntegerType();
                }

                bool isInteger(QualType T) {
                    return T->isIntegerType() || T->isEnumeralType();
                }

                // Bit-width of T as it appears in the rule's "sizeof(T) *
                // CHAR_BIT" formulation - i.e. of the pre-promotion type.
                unsigned bitWidth(QualType T, ASTContext& Ctx) {
                    return static_cast<unsigned>(Ctx.getTypeSize(T));
                }

                // Try to evaluate E as an integer constant expression.
                bool tryConstInt(const Expr* E, ASTContext& Ctx,
                                 llvm::APSInt& Out) {
                    Expr::EvalResult R;
                    if (!E->EvaluateAsInt(R, Ctx)) return false;
                    Out = R.Val.getInt();
                    return true;
                }

                // For the shift-LHS exception: returns true iff
                //   * LHS is a constant integer non-negative value;
                //   * RHS is a constant integer in [0, W-1] where W is the
                //     bit width of LHS's pre-promotion signed type;
                //   * no set bit of LHS, after shifting left by RHS, reaches
                //     bit (W-1) or higher (i.e. the sign bit stays clear).
                // The rule names this exception explicitly for `<<`; for
                // `>>` the same shape (right-shift of a non-negative value)
                // is also well-defined and equally safe, so we apply the
                // exception there too.
                bool shiftLhsLiteralExceptionApplies(const Expr* LHS,
                                                    const Expr* RHS,
                                                    ASTContext& Ctx) {
                    llvm::APSInt L, R;
                    if (!tryConstInt(LHS, Ctx, L)) return false;
                    if (!tryConstInt(RHS, Ctx, R)) return false;
                    if (!L.isNonNegative() || !R.isNonNegative()) return false;
                    QualType LT = preCastType(LHS);
                    unsigned W = bitWidth(LT, Ctx);
                    if (W == 0) return false;
                    if (R.uge(W)) return false;
                    // Compute the shifted value using enough bits to avoid
                    // overflow. We extend L to width 2W and shift left by R,
                    // then check that the result fits in (W-1) bits.
                    unsigned ExtBits = std::max(2u * W, 2u * W);
                    llvm::APInt Lext = L.zext(ExtBits);
                    uint64_t shamt = R.getZExtValue();
                    llvm::APInt Shifted = Lext.shl(shamt);
                    return Shifted.getActiveBits() <= W - 1;
                }

                bool isShiftOpcode(BinaryOperatorKind Op) {
                    return Op == BO_Shl || Op == BO_Shr ||
                           Op == BO_ShlAssign || Op == BO_ShrAssign;
                }

                bool isBinaryBitwiseOpcode(BinaryOperatorKind Op) {
                    return Op == BO_And || Op == BO_Or || Op == BO_Xor ||
                           Op == BO_AndAssign || Op == BO_OrAssign ||
                           Op == BO_XorAssign;
                }

            } // namespace

            SdcBitwiseOperandsUnsignedCheck::SdcBitwiseOperandsUnsignedCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcBitwiseOperandsUnsignedCheck::registerMatchers(
                MatchFinder* Finder) {
                Finder->addMatcher(
                    binaryOperator(unless(isExpansionInSystemHeader()),
                                   anyOf(hasOperatorName("&"),
                                         hasOperatorName("|"),
                                         hasOperatorName("^"),
                                         hasOperatorName("&="),
                                         hasOperatorName("|="),
                                         hasOperatorName("^="),
                                         hasOperatorName("<<"),
                                         hasOperatorName(">>"),
                                         hasOperatorName("<<="),
                                         hasOperatorName(">>=")))
                        .bind("binop"),
                    this);
                Finder->addMatcher(
                    unaryOperator(unless(isExpansionInSystemHeader()),
                                  hasOperatorName("~"))
                        .bind("unop"),
                    this);
            }

            void SdcBitwiseOperandsUnsignedCheck::check(
                const MatchFinder::MatchResult& Result) {
                ASTContext& Ctx = *Result.Context;

                if (const auto* UO =
                        Result.Nodes.getNodeAs<UnaryOperator>("unop")) {
                    QualType OT = preCastType(UO->getSubExpr());
                    if (!isInteger(OT)) return; // overloaded / dependent
                    if (!isUnsigned(OT)) {
                        diag(UO->getOperatorLoc(),
                             "operand of '~' must be of unsigned type "
                             "(operand has type %0)")
                            << UO->getSubExpr()->IgnoreImpCasts()->getType();
                    }
                    return;
                }

                const auto* BO = Result.Nodes.getNodeAs<BinaryOperator>("binop");
                if (!BO) return;
                BinaryOperatorKind Op = BO->getOpcode();

                QualType LT = preCastType(BO->getLHS());
                QualType RT = preCastType(BO->getRHS());
                if (!isInteger(LT) || !isInteger(RT)) return;

                if (isBinaryBitwiseOpcode(Op)) {
                    if (!isUnsigned(LT)) {
                        diag(BO->getOperatorLoc(),
                             "left operand of bitwise operator must be of "
                             "unsigned type (operand has type %0)")
                            << BO->getLHS()->IgnoreImpCasts()->getType();
                    }
                    if (!isUnsigned(RT)) {
                        diag(BO->getOperatorLoc(),
                             "right operand of bitwise operator must be of "
                             "unsigned type (operand has type %0)")
                            << BO->getRHS()->IgnoreImpCasts()->getType();
                    }
                    return;
                }

                if (isShiftOpcode(Op)) {
                    // ----- LHS -----
                    bool LhsOk = isUnsigned(LT);
                    if (!LhsOk) {
                        LhsOk = shiftLhsLiteralExceptionApplies(
                            BO->getLHS(), BO->getRHS(), Ctx);
                    }
                    if (!LhsOk) {
                        diag(BO->getOperatorLoc(),
                             "left operand of shift must be of unsigned type "
                             "(operand has type %0)")
                            << BO->getLHS()->IgnoreImpCasts()->getType();
                    }

                    // ----- RHS -----
                    unsigned W = bitWidth(LT, Ctx);
                    llvm::APSInt RV;
                    if (tryConstInt(BO->getRHS(), Ctx, RV)) {
                        // Constant RHS: must be in [0, W-1].
                        if (!RV.isNonNegative() || (W > 0 && RV.uge(W))) {
                            diag(BO->getOperatorLoc(),
                                 "shift amount %0 is outside the valid range "
                                 "[0, %1] for the left operand's type")
                                << toString(RV, /*Radix=*/10)
                                << static_cast<int>(W ? W - 1 : 0);
                        }
                    } else {
                        // Non-constant RHS: must be unsigned.
                        if (!isUnsigned(RT)) {
                            diag(BO->getOperatorLoc(),
                                 "non-constant right operand of shift must "
                                 "be of unsigned type (operand has type %0)")
                                << BO->getRHS()->IgnoreImpCasts()->getType();
                        }
                    }
                    return;
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
