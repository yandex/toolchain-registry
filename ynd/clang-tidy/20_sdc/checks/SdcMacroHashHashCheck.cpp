#include "SdcMacroHashHashCheck.h"
#include "SdcPreprocessorUtils.h"

#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"

namespace clang {
namespace tidy {
namespace sdc {

SdcMacroHashHashCheck::SdcMacroHashHashCheck(StringRef Name,
                                               ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

namespace {

class MacroHashHashCallbacks : public PPCallbacks {
    SdcMacroHashHashCheck& Check;
    const SourceManager& SM;

public:
    MacroHashHashCallbacks(SdcMacroHashHashCheck& C, const SourceManager& SM)
        : Check(C), SM(SM) {}

    void MacroDefined(const Token& MacroNameTok,
                      const MacroDirective* MD) override {
        if (::sdc::pp::isSystemHeader(MacroNameTok.getLocation(), SM)) return;

        const MacroInfo* MI = MD->getMacroInfo();
        if (!MI->isFunctionLike() || MI->tokens_empty()) return;

        const auto Toks = MI->tokens();
        const unsigned N = Toks.size();

        // Flag the pattern: # param ## ...
        // Tokens: tok::hash  tok::identifier(param)  tok::hashhash
        for (unsigned I = 0; I + 2 < N; ++I) {
            if (!::sdc::pp::isStringify(Toks[I])) continue;
            if (!::sdc::pp::isParam(Toks[I + 1], MI)) continue;
            if (!::sdc::pp::isPaste(Toks[I + 2])) continue;

            Check.diag(Toks[I].getLocation(),
                       "macro parameter following '#' shall not be immediately "
                       "followed by '##' in macro '%0'")
                << MacroNameTok.getIdentifierInfo()->getName();
        }
    }
};

} // namespace

void SdcMacroHashHashCheck::registerPPCallbacks(const SourceManager& SM,
                                                  Preprocessor* PP,
                                                  Preprocessor* /*MEP*/) {
    PP->addPPCallbacks(
        std::make_unique<MacroHashHashCallbacks>(*this, SM));
}

} // namespace sdc
} // namespace tidy
} // namespace clang
