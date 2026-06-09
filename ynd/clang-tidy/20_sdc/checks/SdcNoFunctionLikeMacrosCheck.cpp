#include "SdcNoFunctionLikeMacrosCheck.h"
#include "SdcPreprocessorUtils.h"

#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/Token.h"

namespace clang {
namespace tidy {
namespace sdc {

SdcNoFunctionLikeMacrosCheck::SdcNoFunctionLikeMacrosCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

namespace {

// Returns true when the macro body contains an exception token:
//   - __LINE__, __FILE__, or __func__  (predefined identifiers)
//   - # (stringification) or ## (token-paste) operators
static bool hasException(const MacroInfo* MI) {
    for (const auto& Tok : MI->tokens()) {
        if (::sdc::pp::isStringify(Tok) || ::sdc::pp::isPaste(Tok))
            return true;
        // __func__ is a C++11 keyword (tok::kw___func__).
        if (Tok.is(tok::kw___func__)) return true;
        // __LINE__ and __FILE__ are predefined macros — stored as identifiers.
        if (Tok.is(tok::identifier)) {
            StringRef Name = Tok.getIdentifierInfo()->getName();
            if (Name == "__LINE__" || Name == "__FILE__") return true;
        }
    }
    return false;
}

class FunctionLikeMacroCallbacks : public PPCallbacks {
    SdcNoFunctionLikeMacrosCheck& Check;
    const SourceManager& SM;

public:
    FunctionLikeMacroCallbacks(SdcNoFunctionLikeMacrosCheck& C,
                                const SourceManager& SM)
        : Check(C), SM(SM) {}

    void MacroDefined(const Token& MacroNameTok,
                      const MacroDirective* MD) override {
        if (::sdc::pp::isSystemHeader(MacroNameTok.getLocation(), SM)) return;

        const MacroInfo* MI = MD->getMacroInfo();
        if (!MI->isFunctionLike()) return;
        if (hasException(MI)) return;

        Check.diag(MacroNameTok.getLocation(),
                   "function-like macro '%0' shall not be defined; "
                   "use an inline function instead")
            << MacroNameTok.getIdentifierInfo()->getName();
    }
};

} // namespace

void SdcNoFunctionLikeMacrosCheck::registerPPCallbacks(
    const SourceManager& SM, Preprocessor* PP, Preprocessor* /*MEP*/) {
    PP->addPPCallbacks(
        std::make_unique<FunctionLikeMacroCallbacks>(*this, SM));
}

} // namespace sdc
} // namespace tidy
} // namespace clang
