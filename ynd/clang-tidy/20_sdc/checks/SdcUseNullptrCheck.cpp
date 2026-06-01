#include "SdcUseNullptrCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/Token.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                class NullMacroCallbacks: public PPCallbacks {
                public:
                    NullMacroCallbacks(SdcUseNullptrCheck* Check,
                                       const SourceManager& SM)
                        : Check_(Check), SM_(SM)
                    {
                    }

                    void MacroExpands(const Token& MacroNameTok,
                                      const MacroDefinition& MD,
                                      SourceRange Range,
                                      const MacroArgs* Args) override {
                        (void)MD;
                        (void)Range;
                        (void)Args;
                        const IdentifierInfo* II = MacroNameTok.getIdentifierInfo();
                        if (!II || II->getName() != "NULL") return;
                        // Skip expansions originating from system headers.
                        SourceLocation Loc = MacroNameTok.getLocation();
                        if (SM_.isInSystemHeader(Loc)) return;
                        Check_->diag(Loc,
                                     "use of macro 'NULL' is not permitted; "
                                     "use 'nullptr'");
                    }

                private:
                    SdcUseNullptrCheck* Check_;
                    const SourceManager& SM_;
                };

            } // namespace

            SdcUseNullptrCheck::SdcUseNullptrCheck(StringRef Name,
                                                   ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcUseNullptrCheck::registerPPCallbacks(
                const SourceManager& SM, Preprocessor* PP,
                Preprocessor* /*ModuleExpanderPP*/) {
                PP->addPPCallbacks(
                    std::make_unique<NullMacroCallbacks>(this, SM));
            }

            void SdcUseNullptrCheck::registerMatchers(MatchFinder* Finder) {
                // Implicit conversion to a pointer (or member pointer) whose
                // source is not the literal `nullptr`.
                Finder->addMatcher(
                    implicitCastExpr(
                        unless(isExpansionInSystemHeader()),
                        anyOf(hasCastKind(CK_NullToPointer),
                              hasCastKind(CK_NullToMemberPointer)))
                        .bind("cast"),
                    this);
            }

            void SdcUseNullptrCheck::check(
                const MatchFinder::MatchResult& Result) {
                const auto* CE =
                    Result.Nodes.getNodeAs<ImplicitCastExpr>("cast");
                if (!CE) return;
                const Expr* Sub = CE->getSubExpr()->IgnoreParenImpCasts();
                if (isa<CXXNullPtrLiteralExpr>(Sub)) return;
                // GNU __null builtin is sometimes used as a null pointer
                // literal; treat it the same way clang-tidy's nullptr check
                // does and still flag it - the rule wants nullptr only.
                diag(CE->getExprLoc(),
                     "use 'nullptr' for null-pointer constants instead of an "
                     "integer literal or value-initialization");
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
