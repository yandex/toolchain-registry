#include "SdcIfEndifSameFileCheck.h"
#include "SdcPreprocessorUtils.h"

#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"

namespace clang {
namespace tidy {
namespace sdc {

SdcIfEndifSameFileCheck::SdcIfEndifSameFileCheck(StringRef Name,
                                                   ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

namespace {

class IfEndifCallbacks : public PPCallbacks {
    SdcIfEndifSameFileCheck& Check;
    const SourceManager& SM;

    void checkSameFile(SourceLocation Loc, SourceLocation IfLoc,
                       StringRef Directive) {
        if (::sdc::pp::isSystemHeader(Loc, SM)) return;
        if (!::sdc::pp::inSameFile(Loc, IfLoc, SM))
            Check.diag(Loc,
                       "'#%0' shall be in the same file as its corresponding "
                       "'#if', '#ifdef', or '#ifndef'")
                << Directive;
    }

public:
    IfEndifCallbacks(SdcIfEndifSameFileCheck& C, const SourceManager& SM)
        : Check(C), SM(SM) {}

    void Elif(SourceLocation Loc, SourceRange /*CondRange*/,
              ConditionValueKind /*CV*/, SourceLocation IfLoc) override {
        checkSameFile(Loc, IfLoc, "elif");
    }

    void Else(SourceLocation Loc, SourceLocation IfLoc) override {
        checkSameFile(Loc, IfLoc, "else");
    }

    void Endif(SourceLocation Loc, SourceLocation IfLoc) override {
        checkSameFile(Loc, IfLoc, "endif");
    }
};

} // namespace

void SdcIfEndifSameFileCheck::registerPPCallbacks(const SourceManager& SM,
                                                    Preprocessor* PP,
                                                    Preprocessor* /*MEP*/) {
    PP->addPPCallbacks(std::make_unique<IfEndifCallbacks>(*this, SM));
}

} // namespace sdc
} // namespace tidy
} // namespace clang
