#include "SdcNoOctalConstantsCheck.h"
#include "SdcCodeSelection.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/Stmt.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                bool isProhibitedOctalSpelling(llvm::StringRef SourceText) {
                    if (!SourceText.starts_with("0") || SourceText.size() == 1) {
                        return false;
                    }
                    if (SourceText.starts_with("0x") || SourceText.starts_with("0X") ||
                        SourceText.starts_with("0b") || SourceText.starts_with("0B")) {
                        return false;
                    }

                    // A single numeric zero is permitted even with a standard
                    // integer suffix. Digit separators do not end the numeric
                    // portion: 0'123 and 0'0U are still octal spellings with
                    // additional digits.
                    for (size_t Index = 1; Index < SourceText.size(); ++Index) {
                        const char Ch = SourceText[Index];
                        if (Ch >= '0' && Ch <= '9') return true;
                        if (Ch == '\'') continue;
                        break; // Start of a suffix.
                    }
                    return false;
                }

                class OctalPreprocessorCallbacks : public PPCallbacks {
                public:
                    OctalPreprocessorCallbacks(SdcNoOctalConstantsCheck& Check,
                                               const SourceManager& SM,
                                               const LangOptions& LangOpts)
                        : Check(Check), SM(SM), LangOpts(LangOpts) {}

                    void If(SourceLocation, SourceRange Range,
                            ConditionValueKind ValueKind) override {
                        inspectEvaluatedCondition(Range, ValueKind);
                    }

                    void Elif(SourceLocation, SourceRange Range,
                              ConditionValueKind ValueKind,
                              SourceLocation) override {
                        inspectEvaluatedCondition(Range, ValueKind);
                    }

                private:
                    void inspectEvaluatedCondition(SourceRange Range,
                                                   ConditionValueKind ValueKind) {
                        if (ValueKind == CVK_NotEvaluated || Range.isInvalid()) return;

                        SourceLocation Cur = SM.getSpellingLoc(Range.getBegin());
                        const SourceLocation Last = SM.getSpellingLoc(Range.getEnd());
                        if (Cur.isInvalid() || Last.isInvalid() ||
                            SM.isInSystemHeader(Cur)) {
                            return;
                        }

                        while (!SM.isBeforeInTranslationUnit(Last, Cur)) {
                            Token Tok;
                            if (Lexer::getRawToken(Cur, Tok, SM, LangOpts,
                                                   /*IgnoreWhiteSpace=*/true)) {
                                break;
                            }
                            const SourceLocation Next = Lexer::getLocForEndOfToken(
                                Tok.getLocation(), 0, SM, LangOpts);
                            if (Next.isInvalid() || Next == Cur) break;
                            Cur = Next;

                            if (!Tok.is(tok::numeric_constant)) continue;
                            if (!isWrittenInAnalyzedSource(Tok.getLocation(), SM)) {
                                continue;
                            }
                            const std::string Spelling =
                                Lexer::getSpelling(Tok, SM, LangOpts);
                            if (isProhibitedOctalSpelling(Spelling)) {
                                Check.diag(Tok.getLocation(),
                                           "octal constants shall not be used");
                            }
                        }
                    }

                    SdcNoOctalConstantsCheck& Check;
                    const SourceManager& SM;
                    const LangOptions& LangOpts;
                };

            } // namespace

            SdcNoOctalConstantsCheck::SdcNoOctalConstantsCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoOctalConstantsCheck::registerPPCallbacks(
                const SourceManager& SM, Preprocessor* PP,
                Preprocessor* /*ModuleExpanderPP*/) {
                PP->addPPCallbacks(std::make_unique<OctalPreprocessorCallbacks>(
                    *this, SM, PP->getLangOpts()));
            }

            void SdcNoOctalConstantsCheck::registerMatchers(
                MatchFinder* Finder) {
                // Match integer literals ONLY
                // Character literals with octal escapes are NOT covered by this rule
                Finder->addMatcher(
                    traverse(TK_AsIs,
                             integerLiteral(unless(isExpansionInSystemHeader()))
                                 .bind("integer_literal")),
                    this);
            }

            void SdcNoOctalConstantsCheck::check(
                const MatchFinder::MatchResult& Result) {
                // Check integer literals ONLY
                // Character literals with octal escapes are compliant and not checked
                if (const auto* Literal = Result.Nodes.getNodeAs<IntegerLiteral>("integer_literal")) {
                    checkIntegerLiteral(Literal, Result);
                }
            }

            void SdcNoOctalConstantsCheck::checkIntegerLiteral(
                const IntegerLiteral* Literal, const MatchFinder::MatchResult& Result) {
                const SourceManager& SM = *Result.SourceManager;
                if (!isWrittenInAnalyzedSource(Literal->getBeginLoc(), SM)) {
                    return;
                }
                CharSourceRange Range = CharSourceRange::getTokenRange(
                    SM.getSpellingLoc(Literal->getBeginLoc()),
                    SM.getSpellingLoc(Literal->getEndLoc()));

                // Get the source text for the literal
                llvm::StringRef SourceText = Lexer::getSourceText(
                    Range, SM, Result.Context->getLangOpts());

                if (SourceText.empty()) {
                    return;
                }

                if (isProhibitedOctalSpelling(SourceText)) {
                    for (const Decl* Instance : AnalysisInstances.claim(
                             *Literal, Literal->getBeginLoc(), *Result.Context)) {
                        (void)Instance;
                        diag(Literal->getBeginLoc(),
                             "octal constants shall not be used");
                    }
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
