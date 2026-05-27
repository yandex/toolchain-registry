#include "SdcStringLiteralConcatenationCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/SmallString.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

namespace {

// Return the encoding prefix portion of a string-literal token's spelling.
// The R prefix is explicitly NOT an encoding prefix and is dropped if seen.
// Result is one of: "", "L", "u", "u8", "U".
StringRef encodingPrefix(StringRef Spelling) {
    if (Spelling.starts_with("u8")) {
        return Spelling.substr(0, 2);
    }
    if (!Spelling.empty() &&
        (Spelling[0] == 'L' || Spelling[0] == 'u' || Spelling[0] == 'U')) {
        return Spelling.substr(0, 1);
    }
    return StringRef();
}

// For the diagnostic message; empty prefix has no good single-character
// representation so we spell it out.
std::string prefixForDiag(StringRef P) {
    if (P.empty()) {
        return "<none>";
    }
    return P.str();
}

} // namespace

SdcStringLiteralConcatenationCheck::SdcStringLiteralConcatenationCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcStringLiteralConcatenationCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        stringLiteral(unless(isExpansionInSystemHeader())).bind("sl"),
        this);
}

void SdcStringLiteralConcatenationCheck::check(
    const MatchFinder::MatchResult& Result) {
    const auto* SL = Result.Nodes.getNodeAs<StringLiteral>("sl");
    if (!SL) {
        return;
    }
    const unsigned N = SL->getNumConcatenated();
    if (N < 2) {
        return; // single literal token - nothing to compare
    }

    const SourceManager& SM = *Result.SourceManager;
    const LangOptions& LO = Result.Context->getLangOpts();

    // Lexer::getSpelling returns a StringRef backed by either the SourceManager
    // buffer or by a caller-provided SmallString. To compare prefixes across
    // tokens we materialize each one into an owned std::string.
    auto prefixAt = [&](unsigned i) -> std::string {
        SourceLocation Loc = SL->getStrTokenLoc(i);
        if (Loc.isInvalid()) {
            return std::string();
        }
        Loc = SM.getSpellingLoc(Loc);
        SmallString<32> Buf;
        bool Invalid = false;
        StringRef Spelling = Lexer::getSpelling(Loc, Buf, SM, LO, &Invalid);
        if (Invalid) {
            return std::string();
        }
        return encodingPrefix(Spelling).str();
    };

    std::string First = prefixAt(0);
    for (unsigned i = 1; i < N; ++i) {
        std::string Cur = prefixAt(i);
        if (Cur == First) {
            continue;
        }
        SourceLocation Loc = SL->getStrTokenLoc(i);
        diag(Loc,
             "string literal with encoding prefix '%0' shall not be "
             "concatenated with literal with encoding prefix '%1'")
            << prefixForDiag(Cur) << prefixForDiag(First);
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
