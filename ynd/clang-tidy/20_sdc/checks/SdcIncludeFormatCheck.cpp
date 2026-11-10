#include "SdcIncludeFormatCheck.h"
#include "SdcPreprocessorUtils.h"

#include "clang/Basic/Module.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/Lexer.h"

namespace clang {
namespace tidy {
namespace sdc {

SdcIncludeFormatCheck::SdcIncludeFormatCheck(StringRef Name,
                                              ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

namespace {

class IncludeFormatCallbacks : public PPCallbacks {
    SdcIncludeFormatCheck& Check;
    const SourceManager& SM;
    const LangOptions& LO;

public:
    IncludeFormatCallbacks(SdcIncludeFormatCheck& C, const SourceManager& SM,
                            const LangOptions& LO)
        : Check(C), SM(SM), LO(LO) {}

    void InclusionDirective(SourceLocation HashLoc,
                             const Token& /*IncludeTok*/,
                             StringRef /*FileName*/,
                             bool /*IsAngled*/,
                             CharSourceRange FilenameRange,
                             OptionalFileEntryRef /*File*/,
                             StringRef /*SearchPath*/,
                             StringRef /*RelativePath*/,
                             const Module* /*SuggestedModule*/,
                             bool /*ModuleImported*/,
                             SrcMgr::CharacteristicKind /*FileType*/) override {
        if (::sdc::pp::isSystemHeader(HashLoc, SM)) return;

        // InclusionDirective is called after successful header-name expansion.
        // A macro that produces a valid quoted/angled filename is compliant.
        if (FilenameRange.getBegin().isMacroID()) return;

        // For direct includes, verify the text starts with " or <.
        StringRef Text = Lexer::getSourceText(FilenameRange, SM, LO);
        if (Text.empty()) return;
        if (Text.front() == '"' || Text.front() == '<') return; // Compliant

        Check.diag(HashLoc,
                   "#include shall be followed by <filename> or \"filename\"");
    }
};

} // namespace

void SdcIncludeFormatCheck::registerPPCallbacks(const SourceManager& SM,
                                                  Preprocessor* PP,
                                                  Preprocessor* /*MEP*/) {
    PP->addPPCallbacks(
        std::make_unique<IncludeFormatCallbacks>(*this, SM, PP->getLangOpts()));
}

} // namespace sdc
} // namespace tidy
} // namespace clang
