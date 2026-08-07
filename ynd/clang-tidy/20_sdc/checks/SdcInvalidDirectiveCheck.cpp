#include "SdcInvalidDirectiveCheck.h"
#include "SdcPreprocessorUtils.h"

#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"

#include <cctype>

namespace clang {
namespace tidy {
namespace sdc {

SdcInvalidDirectiveCheck::SdcInvalidDirectiveCheck(StringRef Name,
                                                    ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

namespace {

// C++ standard preprocessing directives (plus the null directive '#').
// Implementation-defined extensions (e.g. #warning) are intentionally excluded
// so that this check stays within the standard-mandated set.
static bool isValidDirectiveName(StringRef Name) {
    if (Name.empty()) return true; // null directive: just '#'
    return Name == "if"      || Name == "ifdef"   || Name == "ifndef" ||
           Name == "elif"    || Name == "else"     || Name == "endif"  ||
           Name == "include" || Name == "define"   || Name == "undef"  ||
           Name == "line"    || Name == "error"    || Name == "pragma";
}

// Translation phase 2 removes a backslash-newline pair before preprocessing
// directives are recognized.  Therefore a physical line following such a
// splice is part of the preceding logical line and cannot start a directive.
static bool isLineSplicedFromPrevious(const char* FileStart,
                                      const char* LineStart) {
    if (LineStart == FileStart || *(LineStart - 1) != '\n') return false;

    const char* BeforeNewline = LineStart - 1;
    // SourceManager buffers can retain CRLF line endings.
    if (BeforeNewline != FileStart && *(BeforeNewline - 1) == '\r') {
        --BeforeNewline;
    }
    return BeforeNewline != FileStart && *(BeforeNewline - 1) == '\\';
}

class InvalidDirectiveCallbacks : public PPCallbacks {
    SdcInvalidDirectiveCheck& Check;
    const SourceManager& SM;

public:
    InvalidDirectiveCallbacks(SdcInvalidDirectiveCheck& C,
                               const SourceManager& SM)
        : Check(C), SM(SM) {}

    // Called for source ranges that are skipped by preprocessing (#if 0 etc.).
    // The rule requires checking EVERY line, even excluded ones.
    void SourceRangeSkipped(SourceRange Range,
                             SourceLocation /*EndifLoc*/) override {
        SourceLocation Begin = Range.getBegin();
        if (::sdc::pp::isSystemHeader(Begin, SM)) return;

        // Get raw source bytes for the skipped region.
        bool Invalid = false;
        const char* BufStart = SM.getCharacterData(Begin, &Invalid);
        if (Invalid) return;
        const char* BufEnd = SM.getCharacterData(Range.getEnd(), &Invalid);
        if (Invalid || BufStart >= BufEnd) return;

        FileID FID = SM.getFileID(Begin);
        unsigned StartOffset = SM.getFileOffset(Begin);
        const char* FileStart = SM.getBufferData(FID, &Invalid).data();
        if (Invalid) return;

        // Scan line by line.
        const char* LineStart = BufStart;
        while (LineStart < BufEnd) {
            // A leading '#' on a continuation line belongs to the preceding
            // logical line (for example, '#param' stringification in a
            // multi-line #define), not to a preprocessing directive.
            if (isLineSplicedFromPrevious(FileStart, LineStart)) {
                while (LineStart < BufEnd && *LineStart != '\n') ++LineStart;
                if (LineStart < BufEnd) ++LineStart;
                continue;
            }

            const char* P = LineStart;

            // Skip horizontal whitespace at line start.
            while (P < BufEnd && (*P == ' ' || *P == '\t')) ++P;

            if (P < BufEnd && *P == '#') {
                ++P;
                // Skip whitespace between # and directive name.
                while (P < BufEnd && (*P == ' ' || *P == '\t')) ++P;

                // Read the directive name (alpha/underscore only).
                const char* DirStart = P;
                while (P < BufEnd && (std::isalpha((unsigned char)*P)
                                      || *P == '_'))
                    ++P;
                StringRef Directive(DirStart, P - DirStart);

                if (!isValidDirectiveName(Directive)) {
                    unsigned Offset = StartOffset
                                    + static_cast<unsigned>(LineStart - BufStart);
                    SourceLocation Loc =
                        SM.getLocForStartOfFile(FID).getLocWithOffset(
                            static_cast<int>(Offset));
                    Check.diag(Loc,
                               "'#%0' is not a valid preprocessing directive")
                        << Directive;
                }
            }

            // Advance to the next line.
            while (LineStart < BufEnd && *LineStart != '\n') ++LineStart;
            if (LineStart < BufEnd) ++LineStart;
        }
    }
};

} // namespace

void SdcInvalidDirectiveCheck::registerPPCallbacks(const SourceManager& SM,
                                                     Preprocessor* PP,
                                                     Preprocessor* /*MEP*/) {
    PP->addPPCallbacks(
        std::make_unique<InvalidDirectiveCallbacks>(*this, SM));
}

} // namespace sdc
} // namespace tidy
} // namespace clang
