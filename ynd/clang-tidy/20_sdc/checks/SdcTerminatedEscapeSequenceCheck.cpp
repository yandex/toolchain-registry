#include "SdcTerminatedEscapeSequenceCheck.h"

#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/SmallString.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

namespace {

bool isOctalDigit(char Character) {
    return Character >= '0' && Character <= '7';
}

bool isHexDigit(char Character) {
    return (Character >= '0' && Character <= '9') ||
           (Character >= 'a' && Character <= 'f') ||
           (Character >= 'A' && Character <= 'F');
}

StringRef dropEncodingPrefix(StringRef Spelling) {
    if (Spelling.starts_with("u8")) {
        return Spelling.drop_front(2);
    }
    if (!Spelling.empty() &&
        (Spelling[0] == 'L' || Spelling[0] == 'u' || Spelling[0] == 'U')) {
        return Spelling.drop_front(1);
    }
    return Spelling;
}

std::optional<StringRef> literalBody(StringRef Spelling) {
    StringRef Unprefixed = dropEncodingPrefix(Spelling);
    if (Unprefixed.starts_with("R\"") || Unprefixed.size() < 2) {
        return std::nullopt;
    }
    const char Quote = Unprefixed.front();
    if ((Quote != '\'' && Quote != '"') || Unprefixed.back() != Quote) {
        return std::nullopt;
    }
    return Unprefixed.substr(1, Unprefixed.size() - 2);
}

struct UnterminatedEscape {
    size_t Offset;
    StringRef Kind;
};

std::optional<UnterminatedEscape> findUnterminatedEscape(StringRef Body) {
    for (size_t Index = 0; Index < Body.size();) {
        if (Body[Index] != '\\' || Index + 1 >= Body.size()) {
            ++Index;
            continue;
        }

        const size_t EscapeOffset = Index;
        const char Marker = Body[Index + 1];
        size_t End = Index + 2;
        StringRef Kind;

        if (isOctalDigit(Marker)) {
            Kind = "octal escape sequence";
            const size_t Limit = std::min(Body.size(), Index + 4);
            while (End < Limit && isOctalDigit(Body[End])) {
                ++End;
            }
        } else if (Marker == 'x') {
            Kind = "hexadecimal escape sequence";
            while (End < Body.size() && isHexDigit(Body[End])) {
                ++End;
            }
        } else if (Marker == 'u' || Marker == 'U') {
            Kind = "universal character name";
            const size_t DigitCount = Marker == 'u' ? 4 : 8;
            End = std::min(Body.size(), End + DigitCount);
        } else {
            Index += 2;
            continue;
        }

        // Termination is lexical and explicit: the sequence must be followed
        // by the end of its literal token or another escape introducer. An
        // adjacent string-literal token therefore provides a valid terminator.
        if (End < Body.size() && Body[End] != '\\') {
            return UnterminatedEscape{EscapeOffset, Kind};
        }
        Index = End;
    }
    return std::nullopt;
}

} // namespace

SdcTerminatedEscapeSequenceCheck::SdcTerminatedEscapeSequenceCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcTerminatedEscapeSequenceCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        stringLiteral(unless(isExpansionInSystemHeader())).bind("string"), this);
    Finder->addMatcher(
        characterLiteral(unless(isExpansionInSystemHeader())).bind("character"),
        this);
}

void SdcTerminatedEscapeSequenceCheck::checkLiteralToken(
    const DynTypedNode& Node, SourceLocation TokenLocation,
    SourceLocation DiagnosticLocation, ASTContext& Context) {
    const SourceManager& SM = Context.getSourceManager();
    const LangOptions& LangOpts = Context.getLangOpts();
    if (TokenLocation.isInvalid() ||
        !isWrittenInAnalyzedSource(TokenLocation, SM)) {
        return;
    }
    const SourceLocation WrittenLocation =
        getUltimateWrittenLocation(TokenLocation, SM);

    SmallString<128> Buffer;
    bool Invalid = false;
    StringRef Spelling =
        Lexer::getSpelling(WrittenLocation, Buffer, SM, LangOpts, &Invalid);
    if (Invalid) {
        return;
    }
    std::optional<StringRef> Body = literalBody(Spelling);
    if (!Body) {
        return;
    }
    std::optional<UnterminatedEscape> Violation =
        findUnterminatedEscape(*Body);
    if (!Violation) {
        return;
    }

    if (TokenLocation.isMacroID()) {
        DiagnosticLocation = SM.getExpansionLoc(TokenLocation);
    }
    for (const Decl* Instance :
         AnalysisInstances.claim(Node, TokenLocation, Context)) {
        (void)Instance;
        diag(DiagnosticLocation,
             "%0 is not terminated; end the literal token or start another "
             "escape immediately after it")
            << Violation->Kind;
        if (TokenLocation.isMacroID() &&
            WrittenLocation != DiagnosticLocation) {
            diag(WrittenLocation, "escape sequence is written here",
                 DiagnosticIDs::Note);
        }
    }
}

void SdcTerminatedEscapeSequenceCheck::check(
    const MatchFinder::MatchResult& Result) {
    if (const auto* String = Result.Nodes.getNodeAs<StringLiteral>("string")) {
        for (unsigned Index = 0; Index < String->getNumConcatenated(); ++Index) {
            checkLiteralToken(DynTypedNode::create(*String),
                              String->getStrTokenLoc(Index),
                              String->getStrTokenLoc(0), *Result.Context);
        }
    } else if (const auto* Character =
                   Result.Nodes.getNodeAs<CharacterLiteral>("character")) {
        checkLiteralToken(DynTypedNode::create(*Character),
                          Character->getLocation(),
                          Character->getLocation(), *Result.Context);
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
