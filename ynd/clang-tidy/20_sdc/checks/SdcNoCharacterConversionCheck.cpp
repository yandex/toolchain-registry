#include "SdcNoCharacterConversionCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                // "Character category" per the rule: plain char, wchar_t,
                // char8_t, char16_t, char32_t. Note that `signed char` and
                // `unsigned char` are NOT character category - they have
                // integral category, which is intentional and matches the
                // rule's "signed char f = 11; // ok" example.
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

                // Walk parents to detect that the cast lives inside an
                // unevaluated operand (sizeof / alignof / noexcept / typeid).
                bool inUnevaluatedContext(const Expr* E, ASTContext& Ctx) {
                    DynTypedNode N = DynTypedNode::create(*E);
                    for (int Depth = 0; Depth < 32; ++Depth) {
                        auto Parents = Ctx.getParents(N);
                        if (Parents.empty()) return false;
                        N = Parents[0];
                        if (N.get<UnaryExprOrTypeTraitExpr>()) return true;
                        if (N.get<CXXNoexceptExpr>()) return true;
                        if (N.get<CXXTypeidExpr>()) return true;
                        if (N.get<FunctionDecl>()) return false;
                        if (N.get<TranslationUnitDecl>()) return false;
                    }
                    return false;
                }

                QualType preCastType(const Expr* E) {
                    return E ? E->IgnoreImpCasts()
                                   ->getType()
                                   .getCanonicalType()
                                   .getUnqualifiedType()
                             : QualType{};
                }

            } // namespace

            SdcNoCharacterConversionCheck::SdcNoCharacterConversionCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoCharacterConversionCheck::registerMatchers(
                MatchFinder* Finder) {
                Finder->addMatcher(
                    castExpr(unless(isExpansionInSystemHeader())).bind("cast"),
                    this);
            }

            void SdcNoCharacterConversionCheck::check(
                const MatchFinder::MatchResult& Result) {
                const auto* CE = Result.Nodes.getNodeAs<CastExpr>("cast");
                if (!CE) return;
                ASTContext& Ctx = *Result.Context;

                QualType DstTy = CE->getType()
                                     .getCanonicalType()
                                     .getUnqualifiedType();
                QualType SrcTy = CE->getSubExpr()
                                     ->getType()
                                     .getCanonicalType()
                                     .getUnqualifiedType();
                if (SrcTy == DstTy) return;

                const bool SrcChar = isCharacterCategory(SrcTy);
                const bool DstChar = isCharacterCategory(DstTy);
                if (!SrcChar && !DstChar) return;

                // (void)c is a discard, not a conversion.
                if (DstTy->isVoidType()) return;

                if (inUnevaluatedContext(CE, Ctx)) return;

                // Equality between two operands of the same character type:
                // clang promotes both to int; exempt by the rule.
                DynTypedNode N = DynTypedNode::create(*CE);
                auto Parents = Ctx.getParents(N);
                if (!Parents.empty()) {
                    if (const auto* BO =
                            Parents[0].get<BinaryOperator>()) {
                        BinaryOperatorKind Op = BO->getOpcode();
                        if (Op == BO_EQ || Op == BO_NE) {
                            QualType L = preCastType(BO->getLHS());
                            QualType R = preCastType(BO->getRHS());
                            if (isCharacterCategory(L) &&
                                isCharacterCategory(R) && L == R) {
                                return;
                            }
                        }
                    }
                }

                // When the cast is inside a macro body, getExprLoc() is a
                // macro expansion location that clang resolves to the outermost
                // call site, pushing the actual code into "expanded from" notes.
                // Report at the spelling location (where the code lives) and
                // add a note pointing to the user's call site instead.
                const SourceManager& SM = *Result.SourceManager;
                SourceLocation DiagLoc = CE->getExprLoc();
                SourceLocation MacroCallSite;
                if (DiagLoc.isMacroID() && SM.isMacroBodyExpansion(DiagLoc)) {
                    MacroCallSite = DiagLoc;
                    while (MacroCallSite.isMacroID())
                        MacroCallSite = SM.getExpansionLoc(MacroCallSite);
                    DiagLoc = SM.getSpellingLoc(DiagLoc);
                }

                if (SrcChar) {
                    diag(DiagLoc,
                         "conversion from character type %0 to %1 is not "
                         "allowed; do not use the numerical value of a "
                         "character")
                        << CE->getSubExpr()->getType()
                        << CE->getType();
                } else {
                    diag(DiagLoc,
                         "conversion from %0 to character type %1 is not "
                         "allowed; do not use the numerical value of a "
                         "character")
                        << CE->getSubExpr()->getType()
                        << CE->getType();
                }
                if (MacroCallSite.isValid())
                    diag(MacroCallSite, "expanded via macro invocation here",
                         DiagnosticIDs::Note);
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
