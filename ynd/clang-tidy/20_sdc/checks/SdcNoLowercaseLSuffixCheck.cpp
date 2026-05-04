#include "SdcNoLowercaseLSuffixCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/Stmt.h"
#include "clang/Lex/Lexer.h"
#include "llvm/Support/raw_ostream.h"
#include <cctype>

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcNoLowercaseLSuffixCheck::SdcNoLowercaseLSuffixCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoLowercaseLSuffixCheck::registerMatchers(
                MatchFinder* Finder) {
                // Match all literal types that can have suffixes
                Finder->addMatcher(
                    integerLiteral().bind("integer_literal"),
                    this);

                Finder->addMatcher(
                    floatLiteral().bind("float_literal"),
                    this);

                Finder->addMatcher(
                    stringLiteral().bind("string_literal"),
                    this);

                Finder->addMatcher(
                    characterLiteral().bind("character_literal"),
                    this);
            }

            void SdcNoLowercaseLSuffixCheck::check(
                const MatchFinder::MatchResult& Result) {
                // Check integer literals
                if (const auto* Literal = Result.Nodes.getNodeAs<IntegerLiteral>("integer_literal")) {
                    checkIntegerLiteral(Literal, Result);
                }

                // Check floating literals
                if (const auto* Literal = Result.Nodes.getNodeAs<FloatingLiteral>("float_literal")) {
                    checkFloatingLiteral(Literal, Result);
                }

                // Check string literals (though they typically don't have numeric suffixes)
                if (const auto* Literal = Result.Nodes.getNodeAs<StringLiteral>("string_literal")) {
                    checkStringLiteral(Literal, Result);
                }

                // Check character literals (though they typically don't have numeric suffixes)
                if (const auto* Literal = Result.Nodes.getNodeAs<CharacterLiteral>("character_literal")) {
                    checkCharacterLiteral(Literal, Result);
                }
            }

            void SdcNoLowercaseLSuffixCheck::checkIntegerLiteral(
                const IntegerLiteral* Literal, const MatchFinder::MatchResult& Result) {
                // Get the source text for the literal including any suffix
                llvm::StringRef SourceText = Lexer::getSourceText(
                    CharSourceRange::getTokenRange(Literal->getSourceRange()),
                    *Result.SourceManager, Result.Context->getLangOpts());

                if (SourceText.empty()) {
                    return;
                }

                // Remove all digit separators (') first, as they don't affect suffix checking
                std::string CleanText;
                for (char c : SourceText) {
                    if (c != '\'') {
                        CleanText += c;
                    }
                }

                llvm::StringRef CleanSourceText(CleanText);

                if (CleanSourceText.empty() || !std::isdigit(CleanSourceText[0])) {
                    return;
                }

                // Check if this has a suffix by looking for non-digit characters after the number
                size_t pos = 0;

                // For hex/binary: skip 0x, 0X, 0b, 0B prefixes
                if (CleanSourceText.starts_with("0x") || CleanSourceText.starts_with("0X")) {
                    pos = 2;
                } else if (CleanSourceText.starts_with("0b") || CleanSourceText.starts_with("0B")) {
                    pos = 2;
                }

                // Skip all hex digits (0-9, a-f, A-F)
                while (pos < CleanSourceText.size()) {
                    char c = CleanSourceText[pos];
                    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                        pos++;
                    } else {
                        break;
                    }
                }

                // If we reached the end, there's no suffix
                if (pos >= CleanSourceText.size()) {
                    return;
                }

                // Check if it's a user-defined literal (starts with underscore)
                if (CleanSourceText[pos] == '_') {
                    return; // User-defined literal - exempt from rule
                }

                // Check if the first character of the suffix is lowercase 'l'
                if (CleanSourceText[pos] == 'l') {
                    // Found lowercase 'l' as first character in suffix
                    diag(Literal->getBeginLoc(),
                         "the lowercase form of L shall not be used as the first character in a literal suffix");
                }
            }

            void SdcNoLowercaseLSuffixCheck::checkFloatingLiteral(
                const FloatingLiteral* Literal, const MatchFinder::MatchResult& Result) {
                // Get the source text for the literal including any suffix
                llvm::StringRef SourceText = Lexer::getSourceText(
                    CharSourceRange::getTokenRange(Literal->getSourceRange()),
                    *Result.SourceManager, Result.Context->getLangOpts());

                if (SourceText.empty()) {
                    return;
                }

                // Remove all digit separators (') first, as they don't affect suffix checking
                std::string CleanText;
                for (char c : SourceText) {
                    if (c != '\'') {
                        CleanText += c;
                    }
                }

                llvm::StringRef CleanSourceText(CleanText);

                char first = CleanSourceText[0];

                if (CleanSourceText.empty()) {
                    return;
                }
                if (!std::isdigit(first) && first != '.' && first != '+' && first != '-') {
                    return;
                }

                // Parse the floating literal to find where the suffix starts
                // Floating literal format: [digits].[digits][e|E[+|-]digits]
                size_t pos = 0;

                // Skip optional sign
                if (CleanSourceText[0] == '+' || CleanSourceText[0] == '-') {
                    pos = 1;
                }

                // Skip integer part digits
                while (pos < CleanSourceText.size() && isdigit(CleanSourceText[pos])) {
                    pos++;
                }

                // Skip decimal point and fractional part if present
                if (pos < CleanSourceText.size() && CleanSourceText[pos] == '.') {
                    pos++;
                    while (pos < CleanSourceText.size() && isdigit(CleanSourceText[pos])) {
                        pos++;
                    }
                }

                // Skip exponent part if present
                if (pos < CleanSourceText.size() && (CleanSourceText[pos] == 'e' || CleanSourceText[pos] == 'E')) {
                    pos++;

                    // Skip optional exponent sign
                    if (pos < CleanSourceText.size() && (CleanSourceText[pos] == '+' || CleanSourceText[pos] == '-')) {
                        pos++;
                    }

                    // Skip exponent digits
                    while (pos < CleanSourceText.size() && isdigit(CleanSourceText[pos])) {
                        pos++;
                    }
                }

                // If we reached the end, there's no suffix
                if (pos >= CleanSourceText.size()) {
                    return;
                }

                // Check if it's a user-defined literal (starts with underscore)
                if (CleanSourceText[pos] == '_') {
                    return; // User-defined literal - exempt from rule
                }

                // Check if the first character of the suffix is lowercase 'l'
                if (CleanSourceText[pos] == 'l') {
                    // Found lowercase 'l' as first character in suffix
                    diag(Literal->getBeginLoc(),
                         "the lowercase form of L shall not be used as the first character in a literal suffix");
                }
            }

            void SdcNoLowercaseLSuffixCheck::checkStringLiteral(
                const StringLiteral* Literal, const MatchFinder::MatchResult& Result) {
                // String literals typically don't have numeric suffixes in standard C++
                // User-defined string literals would start with '_' and are exempt
                // For now, we'll check but this is unlikely to trigger

                llvm::StringRef SourceText = Lexer::getSourceText(
                    CharSourceRange::getTokenRange(Literal->getSourceRange()),
                    *Result.SourceManager, Result.Context->getLangOpts());

                if (SourceText.empty()) {
                    return;
                }

                // String literals end with quote, look for suffix after the closing quote
                // This is complex due to wide string prefixes (L"", u8"", etc.)
                // For simplicity, we'll focus on the main numeric literal types
            }

            void SdcNoLowercaseLSuffixCheck::checkCharacterLiteral(
                const CharacterLiteral* Literal, const MatchFinder::MatchResult& Result) {
                // Character literals typically don't have numeric suffixes in standard C++
                // User-defined character literals would start with '_' and are exempt

                llvm::StringRef SourceText = Lexer::getSourceText(
                    CharSourceRange::getTokenRange(Literal->getSourceRange()),
                    *Result.SourceManager, Result.Context->getLangOpts());

                if (SourceText.empty()) {
                    return;
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
