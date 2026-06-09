#include "SdcIncludeFilenameCharsCheck.h"
#include "SdcPreprocessorUtils.h"

#include "clang/Basic/Module.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"

namespace clang {
namespace tidy {
namespace sdc {

SdcIncludeFilenameCharsCheck::SdcIncludeFilenameCharsCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

namespace {

class FilenameCharsCallbacks : public PPCallbacks {
    SdcIncludeFilenameCharsCheck& Check;
    const SourceManager& SM;

public:
    FilenameCharsCallbacks(SdcIncludeFilenameCharsCheck& C,
                            const SourceManager& SM)
        : Check(C), SM(SM) {}

    void InclusionDirective(SourceLocation HashLoc,
                             const Token& /*IncludeTok*/,
                             StringRef FileName,
                             bool /*IsAngled*/,
                             CharSourceRange /*FilenameRange*/,
                             OptionalFileEntryRef /*File*/,
                             StringRef /*SearchPath*/,
                             StringRef /*RelativePath*/,
                             const Module* /*SuggestedModule*/,
                             bool /*ModuleImported*/,
                             SrcMgr::CharacteristicKind /*FileType*/) override {
        if (::sdc::pp::isSystemHeader(HashLoc, SM)) return;
        if (FileName.empty()) return;

        // Forbidden: single-quote, backslash, block-comment start, line-comment
        StringRef Forbidden[] = {"'", "\\", "/*", "//"};
        for (StringRef F : Forbidden) {
            if (FileName.contains(F)) {
                Check.diag(HashLoc,
                           "header filename '%0' contains a forbidden character "
                           "sequence '%1'")
                    << FileName << F;
                return; // One diagnostic per directive is enough
            }
        }
    }
};

} // namespace

void SdcIncludeFilenameCharsCheck::registerPPCallbacks(
    const SourceManager& SM, Preprocessor* PP, Preprocessor* /*MEP*/) {
    PP->addPPCallbacks(
        std::make_unique<FilenameCharsCallbacks>(*this, SM));
}

} // namespace sdc
} // namespace tidy
} // namespace clang
