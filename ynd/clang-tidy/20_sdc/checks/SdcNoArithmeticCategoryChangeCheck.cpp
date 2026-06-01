#include "SdcNoArithmeticCategoryChangeCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/APSInt.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                enum class Cat { Bool, Char, Signed, Unsigned, Floating, Other };

                bool isCharacterCategory(QualType T) {
                    const auto* BT = T->getAs<BuiltinType>();
                    if (!BT) return false;
                    switch (BT->getKind()) {
                        case BuiltinType::Char_S:
                        case BuiltinType::Char_U:
                        case BuiltinType::WChar_S:
                        case BuiltinType::WChar_U:
                        case BuiltinType::Char8:
                        case BuiltinType::Char16:
                        case BuiltinType::Char32:
                            return true;
                        default:
                            return false;
                    }
                }

                Cat categorize(QualType T) {
                    T = T.getCanonicalType().getUnqualifiedType();
                    if (T->isBooleanType()) return Cat::Bool;
                    if (isCharacterCategory(T)) return Cat::Char;
                    if (T->isUnsignedIntegerType()) return Cat::Unsigned;
                    if (T->isSignedIntegerType()) return Cat::Signed;
                    if (T->isFloatingType()) return Cat::Floating;
                    return Cat::Other;
                }

                const char* catName(Cat C) {
                    switch (C) {
                        case Cat::Bool: return "bool";
                        case Cat::Char: return "character";
                        case Cat::Signed: return "signed integer";
                        case Cat::Unsigned: return "unsigned integer";
                        case Cat::Floating: return "floating";
                        case Cat::Other: return "other";
                    }
                    return "?";
                }

                bool isNonNegativeIntConstant(const Expr* E, ASTContext& Ctx) {
                    Expr::EvalResult R;
                    if (!E->EvaluateAsInt(R, Ctx)) return false;
                    return R.Val.getInt().isNonNegative();
                }

                bool isIntegralConstant(const Expr* E, ASTContext& Ctx) {
                    Expr::EvalResult R;
                    return E->EvaluateAsInt(R, Ctx);
                }

                bool isOperandOfIncDec(const Expr* E, ASTContext& Ctx) {
                    auto Parents = Ctx.getParents(*E);
                    if (Parents.empty()) return false;
                    if (const auto* UO = Parents[0].get<UnaryOperator>()) {
                        return UO->isIncrementDecrementOp();
                    }
                    return false;
                }

                void checkOperand(const Expr* Op, ASTContext& Ctx,
                                  ClangTidyCheck& Check) {
                    if (!Op) return;
                    QualType Final =
                        Op->getType().getCanonicalType().getUnqualifiedType();
                    QualType Orig = Op->IgnoreImpCasts()
                                         ->getType()
                                         .getCanonicalType()
                                         .getUnqualifiedType();
                    if (Orig == Final) return;

                    Cat O = categorize(Orig);
                    Cat F = categorize(Final);
                    if (O == Cat::Other || F == Cat::Other) return;
                    if (O == F) return;

                    const Expr* Stripped = Op->IgnoreImpCasts();

                    // Exception 1: compile-time non-negative signed integral
                    // constant -> unsigned.
                    if (O == Cat::Signed && F == Cat::Unsigned &&
                        isNonNegativeIntConstant(Stripped, Ctx)) {
                        return;
                    }
                    // Exception 2: compile-time integral constant -> floating.
                    if (F == Cat::Floating &&
                        (O == Cat::Signed || O == Cat::Unsigned ||
                         O == Cat::Char || O == Cat::Bool) &&
                        isIntegralConstant(Stripped, Ctx)) {
                        return;
                    }

                    Check.diag(Op->getExprLoc(),
                               "implicit promotion changes operand from %0 "
                               "(%1) to %2 (%3)")
                        << Op->IgnoreImpCasts()->getType() << catName(O)
                        << Op->getType() << catName(F);
                }

            } // namespace

            SdcNoArithmeticCategoryChangeCheck::
                SdcNoArithmeticCategoryChangeCheck(StringRef Name,
                                                   ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoArithmeticCategoryChangeCheck::registerMatchers(
                MatchFinder* Finder) {
                // Binary arithmetic / relational / equality / bitwise / shift
                // operators. Plain `=`, `&&`, `||`, `,` are excluded - those
                // do not go through the usual arithmetic conversions.
                Finder->addMatcher(
                    binaryOperator(unless(isExpansionInSystemHeader()),
                                   anyOf(hasOperatorName("+"),
                                         hasOperatorName("-"),
                                         hasOperatorName("*"),
                                         hasOperatorName("/"),
                                         hasOperatorName("%"),
                                         hasOperatorName("&"),
                                         hasOperatorName("|"),
                                         hasOperatorName("^"),
                                         hasOperatorName("<<"),
                                         hasOperatorName(">>"),
                                         hasOperatorName("<"),
                                         hasOperatorName(">"),
                                         hasOperatorName("<="),
                                         hasOperatorName(">="),
                                         hasOperatorName("=="),
                                         hasOperatorName("!="),
                                         hasOperatorName("+="),
                                         hasOperatorName("-="),
                                         hasOperatorName("*="),
                                         hasOperatorName("/="),
                                         hasOperatorName("%="),
                                         hasOperatorName("&="),
                                         hasOperatorName("|="),
                                         hasOperatorName("^="),
                                         hasOperatorName("<<="),
                                         hasOperatorName(">>=")))
                        .bind("bin"),
                    this);
                // Unary +, -, ~ apply integral promotion to the operand.
                // ++ and -- do as well, but are explicitly exempted.
                Finder->addMatcher(
                    unaryOperator(unless(isExpansionInSystemHeader()),
                                  anyOf(hasOperatorName("+"),
                                        hasOperatorName("-"),
                                        hasOperatorName("~")))
                        .bind("un"),
                    this);
                // Conditional operator: true/false branches go through UAC.
                Finder->addMatcher(
                    conditionalOperator(unless(isExpansionInSystemHeader()))
                        .bind("cond"),
                    this);
            }

            void SdcNoArithmeticCategoryChangeCheck::check(
                const MatchFinder::MatchResult& Result) {
                ASTContext& Ctx = *Result.Context;
                if (const auto* BO =
                        Result.Nodes.getNodeAs<BinaryOperator>("bin")) {
                    checkOperand(BO->getLHS(), Ctx, *this);
                    checkOperand(BO->getRHS(), Ctx, *this);
                    return;
                }
                if (const auto* UO =
                        Result.Nodes.getNodeAs<UnaryOperator>("un")) {
                    checkOperand(UO->getSubExpr(), Ctx, *this);
                    return;
                }
                if (const auto* CO =
                        Result.Nodes.getNodeAs<ConditionalOperator>("cond")) {
                    checkOperand(CO->getTrueExpr(), Ctx, *this);
                    checkOperand(CO->getFalseExpr(), Ctx, *this);
                    return;
                }
                (void)isOperandOfIncDec; // currently unused; reserved.
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
