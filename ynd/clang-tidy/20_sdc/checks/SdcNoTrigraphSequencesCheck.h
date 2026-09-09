#pragma once
#include "bridge_header.h"

namespace clang::tidy::sdc {
class SdcNoTrigraphSequencesCheck final : public ClangTidyCheck {
public:
    SdcNoTrigraphSequencesCheck(StringRef Name, ClangTidyContext *Context)
        : ClangTidyCheck(Name, Context) {}
    void registerPPCallbacks(const SourceManager &, Preprocessor *, Preprocessor *) override;
};
} // namespace clang::tidy::sdc
