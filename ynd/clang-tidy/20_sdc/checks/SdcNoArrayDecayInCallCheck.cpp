#include "SdcNoArrayDecayInCallCheck.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                bool isCharacterType(QualType T) {
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

                bool isPointerToCharacter(QualType T) {
                    if (!T->isPointerType()) return false;
                    return isCharacterType(T->getPointeeType());
                }

                // True if E is a string literal, or a selection between
                // string-literal-like operands (e.g.
                // `cond ? "yes" : "no"`). Every reachable leaf is a string
                // literal, so the decayed array is still guaranteed to carry a
                // terminating sentinel - the same justification as the bare
                // string-literal exemption.
                bool isStringLiteralLike(const Expr* E) {
                    if (!E) return false;
                    E = E->IgnoreParenImpCasts();
                    if (isa<StringLiteral>(E)) return true;
                    if (const auto* CO = dyn_cast<ConditionalOperator>(E)) {
                        return isStringLiteralLike(CO->getTrueExpr()) &&
                               isStringLiteralLike(CO->getFalseExpr());
                    }
                    if (const auto* BCO =
                            dyn_cast<BinaryConditionalOperator>(E)) {
                        // GNU `a ?: b`: the common (true) value reuses the
                        // condition; check both the common and the false arm.
                        return isStringLiteralLike(BCO->getCommon()) &&
                               isStringLiteralLike(BCO->getFalseExpr());
                    }
                    return false;
                }

                // Inspect a single argument expression for array-to-pointer
                // decay. Emits a diagnostic on the offending decay, except
                // when the source is a string literal and the target is a
                // pointer-to-character type.
                void inspectArg(const Expr* Arg, QualType ParamTy,
                                ClangTidyCheck& Check) {
                    if (!Arg) return;
                    // Walk through the chain of implicit casts looking for an
                    // ArrayToPointerDecay; clang may layer qualification or
                    // bit-cast adjustments (e.g. char[] -> char* -> const
                    // char*) on top of the decay itself.
                    const ImplicitCastExpr* Decay = nullptr;
                    const Expr* Top = Arg->IgnoreParens();
                    const Expr* E = Top;
                    while (const auto* ICE = dyn_cast<ImplicitCastExpr>(E)) {
                        if (ICE->getCastKind() == CK_ArrayToPointerDecay) {
                            Decay = ICE;
                            break;
                        }
                        E = ICE->getSubExpr()->IgnoreParens();
                    }
                    if (!Decay) {
                        // When the arms of a `?:` have different array types,
                        // each operand decays independently *inside* the
                        // conditional and the result is already a pointer, so
                        // there is no decay at the top to find. Recurse into
                        // the arms to catch (and exempt) those per-branch
                        // decays. (Same-type arms instead keep an array-typed
                        // result that decays at the top, handled above.)
                        // `E` is the leaf reached after peeling any
                        // non-decay cast adjustments (e.g. a NoOp char* ->
                        // const char* applied on top of the conditional).
                        if (const auto* CO =
                                dyn_cast<ConditionalOperator>(E)) {
                            inspectArg(CO->getTrueExpr(), ParamTy, Check);
                            inspectArg(CO->getFalseExpr(), ParamTy, Check);
                        } else if (const auto* BCO =
                                       dyn_cast<BinaryConditionalOperator>(E)) {
                            inspectArg(BCO->getTrueExpr(), ParamTy, Check);
                            inspectArg(BCO->getFalseExpr(), ParamTy, Check);
                        }
                        return;
                    }
                    const Expr* Sub = Decay->getSubExpr()->IgnoreParens();
                    QualType FinalPtr = Top->getType();
                    if (isStringLiteralLike(Sub) &&
                        isPointerToCharacter(FinalPtr)) {
                        return;
                    }
                    if (ParamTy.isNull()) {
                        ParamTy = FinalPtr;
                    }
                    Check.diag(Arg->getExprLoc(),
                               "array argument %0 decays to pointer %1 at "
                               "the call site; pass the array by reference, "
                               "as std::span, or in another non-decaying form")
                        << Sub->getType() << FinalPtr;
                }

            } // namespace

            SdcNoArrayDecayInCallCheck::SdcNoArrayDecayInCallCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoArrayDecayInCallCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    callExpr(unless(isExpansionInSystemHeader())).bind("call"),
                    this);
                Finder->addMatcher(
                    cxxConstructExpr(unless(isExpansionInSystemHeader()))
                        .bind("ctor"),
                    this);
            }

            void SdcNoArrayDecayInCallCheck::check(
                const MatchFinder::MatchResult& Result) {
                if (const auto* CE = Result.Nodes.getNodeAs<CallExpr>("call")) {
                    const FunctionDecl* FD = CE->getDirectCallee();
                    unsigned NumParams = FD ? FD->getNumParams() : 0;
                    for (unsigned I = 0; I < CE->getNumArgs(); ++I) {
                        QualType ParamTy;
                        if (I < NumParams) {
                            ParamTy = FD->getParamDecl(I)->getType();
                        }
                        inspectArg(CE->getArg(I), ParamTy, *this);
                    }
                    return;
                }
                if (const auto* CC =
                        Result.Nodes.getNodeAs<CXXConstructExpr>("ctor")) {
                    const CXXConstructorDecl* Ctor = CC->getConstructor();
                    for (unsigned I = 0; I < CC->getNumArgs(); ++I) {
                        QualType ParamTy;
                        if (Ctor && I < Ctor->getNumParams()) {
                            ParamTy = Ctor->getParamDecl(I)->getType();
                        }
                        inspectArg(CC->getArg(I), ParamTy, *this);
                    }
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
