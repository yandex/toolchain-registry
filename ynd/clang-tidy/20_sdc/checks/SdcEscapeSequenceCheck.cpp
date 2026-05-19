#include "SdcEscapeSequenceCheck.h"

#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/SmallString.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

namespace {

bool isOctalDigit(char C) {
    return C >= '0' && C <= '7';
}

bool isHexDigit(char C) {
    return (C >= '0' && C <= '9') ||
           (C >= 'a' && C <= 'f') ||
           (C >= 'A' && C <= 'F');
}

// "u8" must be tested before "u".
StringRef dropEncodingPrefix(StringRef S) {
    if (S.starts_with("u8")) {
        return S.drop_front(2);
    }
    if (!S.empty() && (S[0] == 'L' || S[0] == 'u' || S[0] == 'U')) {
        return S.drop_front(1);
    }
    return S;
}

bool isRawStringLiteral(StringRef Spelling) {
    return dropEncodingPrefix(Spelling).starts_with("R\"");
}

// Extract the body between the opening and closing quote.
// Returns nullopt if the spelling looks malformed (shouldn't happen for a
// successfully-lexed literal, but we are defensive).
std::optional<StringRef> literalBody(StringRef Spelling) {
    StringRef S = dropEncodingPrefix(Spelling);
    if (S.size() < 2) {
        return std::nullopt;
    }
    char Quote = S.front();
    if ((Quote != '"' && Quote != '\'') || S.back() != Quote) {
        return std::nullopt;
    }
    return S.substr(1, S.size() - 2);
}

struct EscapeViolation {
    size_t Offset;   // index of the offending `\` within the body
    StringRef What;  // human-readable category for the diagnostic
};

// Walk the body and return the first escape that does not form a
// defined escape sequence or universal character name.
std::optional<EscapeViolation> findInvalidEscape(StringRef Body) {
    size_t i = 0;
    while (i < Body.size()) {
        if (Body[i] != '\\') {
            ++i;
            continue;
        }
        if (i + 1 >= Body.size()) {
            // A successfully-lexed literal cannot end with a bare backslash;
            // guard against it anyway.
            return EscapeViolation{i, "trailing backslash"};
        }
        const char N = Body[i + 1];
        switch (N) {
            case 'n': case 't': case 'v': case 'b': case 'r':
            case 'f': case 'a': case '\\': case '?': case '\'':
            case '"':
                i += 2;
                break;
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                // Octal escape: \<1..3 octal digits>.
                size_t j = i + 2;
                const size_t Max = std::min(i + 4, Body.size());
                while (j < Max && isOctalDigit(Body[j])) {
                    ++j;
                }
                i = j;
                break;
            }
            case 'x': {
                // Hex escape: \x<1..n hex digits>. Empty hex run is invalid.
                size_t j = i + 2;
                if (j >= Body.size() || !isHexDigit(Body[j])) {
                    return EscapeViolation{i, "`\\x` not followed by a hex digit"};
                }
                while (j < Body.size() && isHexDigit(Body[j])) {
                    ++j;
                }
                i = j;
                break;
            }
            case 'u': case 'U': {
                // UCN: \u<4 hex> or \U<8 hex>. Exact length required.
                const size_t Required = (N == 'u') ? 4 : 8;
                if (i + 2 + Required > Body.size()) {
                    return EscapeViolation{i, "truncated universal character name"};
                }
                for (size_t k = 0; k < Required; ++k) {
                    if (!isHexDigit(Body[i + 2 + k])) {
                        return EscapeViolation{i, "malformed universal character name"};
                    }
                }
                i += 2 + Required;
                break;
            }
            default:
                return EscapeViolation{i, "undefined escape sequence"};
        }
    }
    return std::nullopt;
}

} // namespace

SdcEscapeSequenceCheck::SdcEscapeSequenceCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcEscapeSequenceCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        stringLiteral(unless(isExpansionInSystemHeader())).bind("str"),
        this);
    Finder->addMatcher(
        characterLiteral(unless(isExpansionInSystemHeader())).bind("ch"),
        this);
}

void SdcEscapeSequenceCheck::checkLiteralToken(
    SourceLocation Loc, const SourceManager& SM, const LangOptions& LO) {
    if (Loc.isInvalid() || Loc.isMacroID()) {
        // Macro-expanded literals (e.g. __FILE__, stringification) are out
        // of scope: user source for the literal lives in the macro
        // definition. Lexing the spelling reliably across macro
        // boundaries is fragile, and the rule targets literals the user
        // wrote, not ones the preprocessor synthesised.
        return;
    }
    SmallString<128> Buffer;
    bool Invalid = false;
    StringRef Spelling = Lexer::getSpelling(Loc, Buffer, SM, LO, &Invalid);
    if (Invalid || Spelling.empty()) {
        return;
    }
    if (isRawStringLiteral(Spelling)) {
        return;
    }
    auto Body = literalBody(Spelling);
    if (!Body) {
        return;
    }
    auto V = findInvalidEscape(*Body);
    if (!V) {
        return;
    }

    // Map the body offset back to a source location pointing at the
    // offending `\`. Prefix length + opening quote precedes the body.
    const size_t PrefixLen =
        Spelling.size() - dropEncodingPrefix(Spelling).size();
    const size_t BackslashOffset = PrefixLen + 1 + V->Offset;
    SourceLocation EscapeLoc = Loc.getLocWithOffset(BackslashOffset);

    diag(EscapeLoc,
         "%0 in character/string literal; `\\` shall only form a defined "
         "escape sequence or universal character name")
        << V->What;
}

void SdcEscapeSequenceCheck::check(const MatchFinder::MatchResult& Result) {
    const SourceManager& SM = *Result.SourceManager;
    const LangOptions& LO = Result.Context->getLangOpts();

    if (const auto* SL = Result.Nodes.getNodeAs<StringLiteral>("str")) {
        // Concatenated literals ("foo" "bar") are a single AST node but
        // multiple source tokens; check each piece independently so a bad
        // escape lands on the correct piece's line.
        for (unsigned i = 0; i < SL->getNumConcatenated(); ++i) {
            checkLiteralToken(SL->getStrTokenLoc(i), SM, LO);
        }
        return;
    }
    if (const auto* CL = Result.Nodes.getNodeAs<CharacterLiteral>("ch")) {
        checkLiteralToken(CL->getLocation(), SM, LO);
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
