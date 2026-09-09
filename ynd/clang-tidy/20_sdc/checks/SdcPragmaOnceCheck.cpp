#include "SdcPragmaOnceCheck.h"
#include "SdcPreprocessorFileCollector.h"
namespace clang::tidy::sdc {
namespace {
class Callbacks final : public PreprocessorFileCollector {
public:
    Callbacks(SdcPragmaOnceCheck &Check, Preprocessor &PP)
        : PreprocessorFileCollector(PP), Check(Check) {}
    void EndOfMainFile() override {
        for (FileID FID : Files) {
            auto Start = SM.getLocForStartOfFile(FID);
                auto FE = SM.getFileEntryRefForID(FID);
                if (FID != SM.getMainFileID() && FE && !PP.getHeaderSearchInfo().getFileInfo(*FE).isPragmaOnce)
                    Check.diag(Start, "protect this header with #pragma once (project header-protection policy)");
        }
    }
private:
    SdcPragmaOnceCheck &Check;
};
} // namespace
void SdcPragmaOnceCheck::registerPPCallbacks(const SourceManager &, Preprocessor *PP, Preprocessor *) {
    PP->addPPCallbacks(std::make_unique<Callbacks>(*this, *PP));
}
} // namespace clang::tidy::sdc
