#include "SdcIntegerLiteralSuffix.h"

#include "clang/Lex/Lexer.h"
#include "llvm/ADT/StringRef.h"

#include <cctype>
#include <string>

namespace clang {
namespace tidy {
namespace sdc {

ParsedIntegerLiteralSuffix parseIntegerLiteralSuffix(
    const IntegerLiteral* Literal,
    const SourceManager& SM,
    const LangOptions& LO) {
    ParsedIntegerLiteralSuffix Out;

    llvm::StringRef SourceText = Lexer::getSourceText(
        CharSourceRange::getTokenRange(Literal->getSourceRange()), SM, LO);
    if (SourceText.empty()) {
        return Out;
    }

    // Digit separators are spelled inside the integer-literal token; drop
    // them before scanning so suffix detection works for `1'000'000ULL`.
    std::string Cleaned;
    Cleaned.reserve(SourceText.size());
    for (char c : SourceText) {
        if (c != '\'') {
            Cleaned += c;
        }
    }
    llvm::StringRef T(Cleaned);
    if (T.empty() || !std::isdigit(static_cast<unsigned char>(T[0]))) {
        return Out;
    }

    size_t pos = 0;
    if (T.starts_with("0x") || T.starts_with("0X")) {
        Out.base = ParsedIntegerLiteralSuffix::Base::Hex;
        pos = 2;
    } else if (T.starts_with("0b") || T.starts_with("0B")) {
        Out.base = ParsedIntegerLiteralSuffix::Base::Binary;
        pos = 2;
    } else if (T.size() > 1 && T[0] == '0' &&
               std::isdigit(static_cast<unsigned char>(T[1]))) {
        // Leading 0 followed by another digit is an octal literal. A bare
        // "0" is conventionally octal too but has no suffix to matter.
        Out.base = ParsedIntegerLiteralSuffix::Base::Octal;
    } else {
        Out.base = ParsedIntegerLiteralSuffix::Base::Decimal;
    }

    // Skip the digit body. Using the hex-digit superset is safe across all
    // bases because no integer-literal suffix character (u/U/l/L/_) falls in
    // [a-fA-F].
    while (pos < T.size()) {
        char c = T[pos];
        if ((c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F')) {
            ++pos;
        } else {
            break;
        }
    }

    if (pos >= T.size()) {
        // No suffix at all - that's a successful parse with empty suffix.
        Out.valid = true;
        return Out;
    }

    // User-defined-integer-literal: suffix-identifier starts with `_`.
    if (T[pos] == '_') {
        return Out;
    }

    for (size_t i = pos; i < T.size(); ++i) {
        char c = T[i];
        if (c == 'u' || c == 'U') {
            Out.hasU = true;
        } else if (c == 'l' || c == 'L') {
            ++Out.lCount;
        } else {
            // Unknown character in suffix - bail rather than misclassify.
            return Out;
        }
    }
    Out.valid = true;
    return Out;
}

} // namespace sdc
} // namespace tidy
} // namespace clang
