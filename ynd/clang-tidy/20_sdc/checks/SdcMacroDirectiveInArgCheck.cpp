#include "SdcMacroDirectiveInArgCheck.h"
#include "SdcPreprocessorUtils.h"

#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"

#include <cctype>

namespace clang {
namespace tidy {
namespace sdc {

SdcMacroDirectiveInArgCheck::SdcMacroDirectiveInArgCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

namespace {

// Valid C++ standard directive names (same set as SdcInvalidDirectiveCheck).
static bool isDirectiveName(StringRef Name) {
    return Name == "if"      || Name == "ifdef"   || Name == "ifndef" ||
           Name == "elif"    || Name == "else"     || Name == "endif"  ||
           Name == "include" || Name == "define"   || Name == "undef"  ||
           Name == "line"    || Name == "error"    || Name == "pragma";
}

// Scan the raw source text of the macro invocation's argument list for any
// line that begins with '#' followed by a known directive keyword.
// Such tokens look like preprocessing directives and are non-compliant inside
// macro argument lists.
static void checkRawSource(SdcMacroDirectiveInArgCheck& Check,
                             StringRef MacroName,
                             SourceRange Range,
                             const SourceManager& SM) {
    // Only scan file-level (non-macro) source.
    SourceLocation Begin = Range.getBegin();
    if (Begin.isMacroID()) return;

    bool Invalid = false;
    const char* BufStart = SM.getCharacterData(Begin, &Invalid);
    if (Invalid) return;
    const char* BufEnd = SM.getCharacterData(Range.getEnd(), &Invalid);
    if (Invalid || BufStart >= BufEnd) return;

    FileID FID = SM.getFileID(Begin);
    unsigned StartOffset = SM.getFileOffset(Begin);

    // Walk line-by-line, looking for `#directive` patterns INSIDE the argument
    // (i.e. after an opening paren and before the matching closing paren).
    int ParenDepth = 0;
    const char* LineStart = BufStart;

    while (LineStart < BufEnd) {
        const char* P = LineStart;

        // Track paren depth character by character within the line.
        while (P < BufEnd && *P != '\n') {
            if (*P == '(') ParenDepth++;
            else if (*P == ')') ParenDepth--;
            P++;
        }

        // Now re-scan the line for a directive-like pattern when inside args.
        // We check: optional whitespace, then '#', then optional whitespace,
        // then a known directive keyword — at a point where paren depth > 0.
        //
        // Simple heuristic: skip whitespace, check for '#', read identifier.
        const char* LP = LineStart;
        while (LP < BufEnd && (*LP == ' ' || *LP == '\t')) LP++;

        if (LP < BufEnd && *LP == '#') {
            // We're at a '#' — check if it looks like a directive.
            const char* After = LP + 1;
            while (After < BufEnd && (*After == ' ' || *After == '\t')) After++;

            const char* DirStart = After;
            while (After < BufEnd &&
                   (std::isalpha((unsigned char)*After) || *After == '_'))
                After++;
            StringRef Directive(DirStart, After - DirStart);

            // Only flag if inside parens AND it's a known directive.
            // (OuterParenDepth at the START of this line, approximated by the
            //  depth BEFORE processing this line — use a second pass for accuracy.)
            if (!Directive.empty() && isDirectiveName(Directive)) {
                // Compute the paren depth at this line's start by re-counting
                // from BufStart to LineStart.
                int Depth = 0;
                for (const char* Q = BufStart; Q < LineStart; ++Q) {
                    if (*Q == '(') Depth++;
                    else if (*Q == ')') Depth--;
                }
                if (Depth > 0) { // We're inside a macro argument list
                    unsigned Offset = StartOffset +
                        static_cast<unsigned>(LP - BufStart);
                    SourceLocation Loc =
                        SM.getLocForStartOfFile(FID).getLocWithOffset(
                            static_cast<int>(Offset));
                    Check.diag(Loc,
                                "token '#%0' resembles a preprocessing "
                                "directive and shall not appear within "
                                "a macro argument of '%1'")
                        << Directive << MacroName;
                }
            }
        }

        // Move to next line.
        while (LineStart < BufEnd && *LineStart != '\n') LineStart++;
        if (LineStart < BufEnd) LineStart++;
    }
}

class DirectiveInArgCallbacks : public PPCallbacks {
    SdcMacroDirectiveInArgCheck& Check;
    const SourceManager& SM;

public:
    DirectiveInArgCallbacks(SdcMacroDirectiveInArgCheck& C,
                             const SourceManager& SM)
        : Check(C), SM(SM) {}

    void MacroExpands(const Token& MacroNameTok, const MacroDefinition& /*MD*/,
                      SourceRange Range,
                      const MacroArgs* Args) override {
        if (!Args) return; // Object-like macro
        if (::sdc::pp::isSystemHeader(MacroNameTok.getLocation(), SM)) return;

        checkRawSource(Check, MacroNameTok.getIdentifierInfo()->getName(), Range, SM);
    }
};

} // namespace

void SdcMacroDirectiveInArgCheck::registerPPCallbacks(const SourceManager& SM,
                                                        Preprocessor* PP,
                                                        Preprocessor* /*MEP*/) {
    PP->addPPCallbacks(
        std::make_unique<DirectiveInArgCallbacks>(*this, SM));
}

} // namespace sdc
} // namespace tidy
} // namespace clang
