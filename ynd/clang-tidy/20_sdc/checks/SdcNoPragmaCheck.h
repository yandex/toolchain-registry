#pragma once
#include "bridge_header.h"

namespace clang::tidy::sdc {
class SdcNoPragmaCheck final : public ClangTidyCheck {
public:
    SdcNoPragmaCheck(StringRef Name, ClangTidyContext *Context)
        : ClangTidyCheck(Name, Context) {}
    void registerPPCallbacks(const SourceManager &, Preprocessor *, Preprocessor *) override;
};
} // namespace clang::tidy::sdc
