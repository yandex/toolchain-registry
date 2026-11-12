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

                class RedundantParenthesesPPCallbacks: public PPCallbacks {
                public:
                    explicit RedundantParenthesesPPCallbacks(SdcBlockScopeRedundantParenthesesCheck* Check)
                        : Check_(Check)
                    {
                    }

                    void MacroDefined(const Token& MacroNameTok, const MacroDirective* MD) override {
                        if (!MD) return;
                        const MacroInfo* Macro = MD->getMacroInfo();
                        if (!Macro || !Macro->isFunctionLike()) return;
                        auto Tokens = Macro->tokens();
                        for (size_t I = 1; I + 1 < Tokens.size(); ++I) {
                            const auto* Identifier = Tokens[I].getIdentifierInfo();
                            if (Identifier && Macro->getParameterNum(Identifier) >= 0 &&
                                Tokens[I - 1].is(tok::l_paren) && Tokens[I + 1].is(tok::r_paren))
                                Check_->recordParenParameter(Tokens[I].getLocation());
                        }
                    }

                private:
                    SdcBlockScopeRedundantParenthesesCheck* Check_;
                };

                bool hasRedundantParensAroundName(const VarDecl* Variable,
                                                  const SourceManager& SM, const LangOptions& LangOpts,
                                                  const SdcBlockScopeRedundantParenthesesCheck& Check) {
                    SourceLocation Loc = Variable->getLocation();
                    if (Loc.isInvalid()) {
                        return false;
                    }

                    StringRef Name = Variable->getName();
                    // A name-less VarDecl (e.g. the synthetic outer decl of a
                    // structured binding `auto [a, b] = ...`) has an empty
                    // name; there is no declarator identifier to inspect
                    // and the `[a, b]` syntax is required, not optional.
                    if (Name.empty()) {
                        return false;
                    }

                    // Parenthesizing a whole statement/lambda argument does
                    // not parenthesize the names declared inside it. Only a
                    // complete single-token argument can supply the name.
                    SourceLocation Expansion = Loc;
                    while (Expansion.isMacroID()) {
                        if (SM.isMacroArgExpansion(Expansion)) {
                            unsigned Length = Lexer::MeasureTokenLength(
                                SM.getSpellingLoc(Expansion), SM, LangOpts);
                            bool Whole = SM.isAtStartOfImmediateMacroExpansion(Expansion) &&
                                SM.isAtEndOfImmediateMacroExpansion(Expansion.getLocWithOffset(Length));
                            SourceLocation Parameter = SM.getSpellingLoc(
                                SM.getImmediateExpansionRange(Expansion).getBegin());
                            // The nearest complete argument supplies the
                            // declarator name. Outer forwarding-call parentheses
                            // have already been consumed by preprocessing.
                            if (Whole) return Check.isParenParameter(Parameter);
                        }
                        Expansion = SM.getImmediateSpellingLoc(Expansion);
                    }
                    // For literal names (including inside macro blocks), inspect the source
                    // buffer characters immediately adjacent to the name token.
                    //
                    // All previous range-based approaches accumulated fragile
                    // heuristics (BeginLoc spanning macro definitions, EndLoc
                    // spanning lambda bodies, macro-attribute 256-byte guards…).
                    // The fundamental question is simply:
                    //   is '(' the token before the name, and ')' after it?
                    // Reading from the file buffer answers this in O(name.size())
                    // with no range boundary issues at all.
                    {
                        SourceLocation SpellingLoc = SM.getSpellingLoc(Loc);
                        if (!SpellingLoc.isValid()) return false;

                        bool Invalid = false;
                        StringRef Buf = SM.getBufferData(SM.getFileID(SpellingLoc), &Invalid);
                        if (Invalid || Buf.empty()) return false;

                        unsigned NameOff = SM.getFileOffset(SpellingLoc);
                        if (NameOff + Name.size() > Buf.size()) return false;

                        // Find the non-whitespace character immediately before the name.
                        int Before = static_cast<int>(NameOff) - 1;
                        while (Before >= 0 &&
                               std::isspace(static_cast<unsigned char>(Buf[Before])))
                            --Before;
                        if (Before < 0 || Buf[Before] != '(') return false;

                        // Find the non-whitespace character immediately after the name.
                        size_t After = NameOff + Name.size();
                        while (After < Buf.size() &&
                               std::isspace(static_cast<unsigned char>(Buf[After])))
                            ++After;
                        return After < Buf.size() && Buf[After] == ')';
                    }
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

                if (!hasRedundantParensAroundName(Variable, *Result.SourceManager,
                                                  Result.Context->getLangOpts(), *this)) {
                    return;
                }

                diag(Variable->getLocation(), "block scope object declarations shall not use redundant parentheses around the object name");
            }

            void SdcBlockScopeRedundantParenthesesCheck::recordParenParameter(SourceLocation Location) {
                ParenParameters_.insert(Location.getRawEncoding());
            }

            bool SdcBlockScopeRedundantParenthesesCheck::isParenParameter(SourceLocation Location) const {
                return ParenParameters_.count(Location.getRawEncoding()) != 0;
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
