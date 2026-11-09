#pragma once
#include "SdcPolicyDiagnostic.h"

namespace clang::tidy::sdc {
class SdcNoPragmaCheck final : public SdcPolicyCheck {
public:
    SdcNoPragmaCheck(StringRef Name, ClangTidyContext *Context)
        : SdcPolicyCheck(Name, Context) {}
    void registerPPCallbacks(const SourceManager &, Preprocessor *, Preprocessor *) override;
};
} // namespace clang::tidy::sdc
