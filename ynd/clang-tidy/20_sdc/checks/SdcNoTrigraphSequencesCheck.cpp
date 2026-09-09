#include "SdcNoTrigraphSequencesCheck.h"
#include "SdcPreprocessorFileCollector.h"
namespace clang::tidy::sdc {
namespace {
class Callbacks final : public PreprocessorFileCollector {
public:
    Callbacks(SdcNoTrigraphSequencesCheck &Check, Preprocessor &PP)
        : PreprocessorFileCollector(PP), Check(Check) {}
    void EndOfMainFile() override {
        for (FileID FID : Files) {
            auto Start = SM.getLocForStartOfFile(FID);
            bool Invalid = false;
            StringRef Text = SM.getBufferData(FID, &Invalid);
            if (Invalid) continue;
                // This explicitly lexical rule includes comments, inactive
                // branches and dormant replacement lists.
                for (size_t I = 0; I + 2 < Text.size(); ++I)
                    if (Text[I] == '?' && Text[I+1] == '?' && StringRef("=/'()!<>-").contains(Text[I+2]))
                        Check.diag(Start.getLocWithOffset(I), "do not write trigraph-like character sequences");
        }
    }
private:
    SdcNoTrigraphSequencesCheck &Check;
};
} // namespace
void SdcNoTrigraphSequencesCheck::registerPPCallbacks(const SourceManager &, Preprocessor *PP, Preprocessor *) {
    PP->addPPCallbacks(std::make_unique<Callbacks>(*this, *PP));
}
} // namespace clang::tidy::sdc
