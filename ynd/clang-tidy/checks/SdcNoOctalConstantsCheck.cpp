#include "SdcNoOctalConstantsCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/Stmt.h"
#include "clang/Lex/Lexer.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcNoOctalConstantsCheck::SdcNoOctalConstantsCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoOctalConstantsCheck::registerMatchers(
                MatchFinder* Finder) {
                // Match integer literals ONLY
                // Character literals with octal escapes are NOT covered by this rule
                Finder->addMatcher(
                    integerLiteral().bind("integer_literal"),
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
                // Get the source text for the literal
                llvm::StringRef SourceText = Lexer::getSourceText(
                    CharSourceRange::getTokenRange(Literal->getSourceRange()),
                    *Result.SourceManager, Result.Context->getLangOpts());

                if (SourceText.empty()) {
                    return;
                }

                // Check if it's an octal constant
                if (SourceText.starts_with("0") && SourceText.size() > 1) {
                    // Check if it's a hexadecimal (starts with 0x or 0X)
                    if (SourceText.starts_with("0x") || SourceText.starts_with("0X")) {
                        return;
                    }

                    // Check if it's a binary (starts with 0b or 0B)
                    if (SourceText.starts_with("0b") || SourceText.starts_with("0B")) {
                        return;
                    }

                    // Check the exception: single zero with optional suffix is allowed
                    // (e.g., 0L, 0U, 0UL, 0LL, etc.)
                    // But NOT octal notation like 00, 000, etc.
                    // Find where the numeric part ends (before any suffix like L, U, LL, ULL)
                    size_t numericEnd = 1; // Start after the first '0'
                    while (numericEnd < SourceText.size() &&
                           SourceText[numericEnd] >= '0' && SourceText[numericEnd] <= '9') {
                        numericEnd++;
                    }

                    // If the numeric part is just "0" (no additional digits), it's allowed
                    if (numericEnd == 1) {
                        return; // Just "0" followed by optional suffix
                    }

                    // It's an octal constant - report the violation
                    diag(Literal->getBeginLoc(),
                         "octal constants shall not be used");
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
