#include "SdcBlockScopeRedundantParenthesesCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/Token.h"

#include <cctype>

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {
            namespace {

                bool isIdentifierChar(char C) {
                    return std::isalnum(static_cast<unsigned char>(C)) || C == '_';
                }

                bool sourceHasParenWrappedName(StringRef Source, StringRef Name) {
                    size_t Pos = Source.find(Name);
                    while (Pos != StringRef::npos) {
                        bool LeftBoundary = Pos == 0 || !isIdentifierChar(Source[Pos - 1]);
                        size_t End = Pos + Name.size();
                        bool RightBoundary = End >= Source.size() || !isIdentifierChar(Source[End]);
                        if (LeftBoundary && RightBoundary) {
                            int Prev = static_cast<int>(Pos) - 1;
                            while (Prev >= 0 && std::isspace(static_cast<unsigned char>(Source[Prev]))) {
                                --Prev;
                            }

                            size_t Next = End;
                            while (Next < Source.size() && std::isspace(static_cast<unsigned char>(Source[Next]))) {
                                ++Next;
                            }

                            if (Prev >= 0 && Next < Source.size() && Source[Prev] == '(' && Source[Next] == ')') {
                                return true;
                            }
                        }
                        Pos = Source.find(Name, Pos + 1);
                    }
                    return false;
                }

                bool rangeHasParenWrappedName(SourceLocation Begin, SourceLocation End, StringRef Name,
                                              const SourceManager& SM, const LangOptions& LangOpts) {
                    if (Begin.isInvalid() || End.isInvalid()) {
                        return false;
                    }

                    CharSourceRange Range = CharSourceRange::getTokenRange(
                        SM.getSpellingLoc(Begin), SM.getSpellingLoc(End));
                    StringRef Source = Lexer::getSourceText(Range, SM, LangOpts);
                    return !Source.empty() && sourceHasParenWrappedName(Source, Name);
                }

                bool macroReplacementHasParenWrappedParameter(const MacroInfo* Macro) {
                    if (!Macro || !Macro->isFunctionLike()) {
                        return false;
                    }

                    const auto isMacroParameter = [&](const Token& Token) {
                        const IdentifierInfo* Identifier = Token.getIdentifierInfo();
                        return Identifier && Macro->getParameterNum(Identifier) >= 0;
                    };

                    auto Tokens = Macro->tokens();
                    for (size_t I = 1; I + 1 < Tokens.size(); ++I) {
                        if (Tokens[I - 1].is(tok::l_paren) && isMacroParameter(Tokens[I]) &&
                            Tokens[I + 1].is(tok::r_paren)) {
                            return true;
                        }
                    }
                    return false;
                }

                class RedundantParenthesesPPCallbacks: public PPCallbacks {
                public:
                    explicit RedundantParenthesesPPCallbacks(SdcBlockScopeRedundantParenthesesCheck* Check)
                        : Check_(Check)
                    {
                    }

                    void MacroDefined(const Token& MacroNameTok, const MacroDirective* MD) override {
                        if (!MD || !macroReplacementHasParenWrappedParameter(MD->getMacroInfo())) {
                            return;
                        }

                        if (const IdentifierInfo* Identifier = MacroNameTok.getIdentifierInfo()) {
                            Check_->recordMacroWithRedundantParens(Identifier->getName());
                        }
                    }

                private:
                    SdcBlockScopeRedundantParenthesesCheck* Check_;
                };

                bool hasRedundantParensAroundName(const VarDecl* Variable, StringRef MacroName,
                                                  const SourceManager& SM, const LangOptions& LangOpts,
                                                  const SdcBlockScopeRedundantParenthesesCheck& Check) {
                    SourceLocation Loc = Variable->getLocation();
                    if (Loc.isInvalid()) {
                        return false;
                    }

                    StringRef Name = Variable->getName();
                    // A name-less VarDecl (e.g. the synthetic outer decl of a
                    // structured binding `auto [a, b] = ...`) has an empty
                    // name; the substring search would find spurious matches
                    // and the `[a, b]` syntax is required, not optional.
                    if (Name.empty()) {
                        return false;
                    }

                    // Path 1 (macro-driven): only apply when this variable's
                    // *name token* itself originated as a macro argument that
                    // landed in a `( param )` slot of the macro's body. Without
                    // the argument-origin check we would flag every variable
                    // declared inside any macro whose body happens to contain
                    // a `( param )` token sequence anywhere.
                    if (!MacroName.empty() && Loc.isMacroID() &&
                        SM.isMacroArgExpansion(Loc) &&
                        Check.macroHasRedundantParens(MacroName)) {
                        return true;
                    }

                    return rangeHasParenWrappedName(Variable->getBeginLoc(), Variable->getEndLoc(), Name, SM, LangOpts) ||
                           (Loc.isMacroID() &&
                            rangeHasParenWrappedName(SM.getExpansionLoc(Loc), SM.getExpansionLoc(Loc), Name, SM, LangOpts));
                }

            } // namespace

            SdcBlockScopeRedundantParenthesesCheck::SdcBlockScopeRedundantParenthesesCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcBlockScopeRedundantParenthesesCheck::registerPPCallbacks(
                const SourceManager& /*SM*/, Preprocessor* PP, Preprocessor* /*ModuleExpanderPP*/) {
                PP->addPPCallbacks(std::make_unique<RedundantParenthesesPPCallbacks>(this));
            }

            void SdcBlockScopeRedundantParenthesesCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    varDecl(
                        unless(isExpansionInSystemHeader()),
                        hasParent(declStmt(hasAncestor(compoundStmt())).bind("statement")))
                        .bind("variable"),
                    this);
            }

            void SdcBlockScopeRedundantParenthesesCheck::check(const MatchFinder::MatchResult& Result) {
                const auto* Variable = Result.Nodes.getNodeAs<VarDecl>("variable");
                if (!Variable || !Variable->getType()->isObjectType()) {
                    return;
                }

                StringRef MacroName;
                if (Variable->getLocation().isMacroID()) {
                    MacroName = Lexer::getImmediateMacroName(Variable->getLocation(), *Result.SourceManager,
                                                             Result.Context->getLangOpts());
                }

                if (!hasRedundantParensAroundName(Variable, MacroName, *Result.SourceManager,
                                                  Result.Context->getLangOpts(), *this)) {
                    return;
                }

                diag(Variable->getLocation(), "block scope object declarations shall not use redundant parentheses around the object name");
            }

            void SdcBlockScopeRedundantParenthesesCheck::recordMacroWithRedundantParens(StringRef Name) {
                MacrosWithRedundantParens_.insert(Name.str());
            }

            bool SdcBlockScopeRedundantParenthesesCheck::macroHasRedundantParens(StringRef Name) const {
                return MacrosWithRedundantParens_.find(Name.str()) != MacrosWithRedundantParens_.end();
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
