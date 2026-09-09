#include "SdcNoMacroOperatorsCheck.h"
#include "SdcPreprocessorFileCollector.h"
namespace clang::tidy::sdc {
namespace {
class Callbacks final : public PreprocessorFileCollector {
public:
    Callbacks(SdcNoMacroOperatorsCheck &Check, Preprocessor &PP)
        : PreprocessorFileCollector(PP), Check(Check) {}
    void MacroDefined(const Token &, const MacroDirective *MD) override {
        // The macro-operator policy explicitly targets preprocessing operators in a macro
        // definition, including an unexpanded definition.
        for (const auto &T : MD->getMacroInfo()->tokens())
            if (T.isOneOf(tok::hash, tok::hashhash) && isWrittenInAnalyzedSource(T.getLocation(), SM))
                Check.diag(T.getLocation(), "do not use the # or ## preprocessor operators");
    }
private:
    SdcNoMacroOperatorsCheck &Check;
};
} // namespace
void SdcNoMacroOperatorsCheck::registerPPCallbacks(const SourceManager &, Preprocessor *PP, Preprocessor *) {
    PP->addPPCallbacks(std::make_unique<Callbacks>(*this, *PP));
}
} // namespace clang::tidy::sdc
