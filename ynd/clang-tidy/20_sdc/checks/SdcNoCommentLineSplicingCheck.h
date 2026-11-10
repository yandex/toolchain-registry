#pragma once
#include "SdcPolicyDiagnostic.h"

namespace clang::tidy::sdc {
class SdcNoCommentLineSplicingCheck final : public SdcPolicyCheck {
public:
    SdcNoCommentLineSplicingCheck(StringRef Name, ClangTidyContext* Context)
        : SdcPolicyCheck(Name, Context) {}
    void registerPPCallbacks(const SourceManager&, Preprocessor*, Preprocessor*) override;
};
}
