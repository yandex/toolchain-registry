#pragma once
#include "SdcPolicyDiagnostic.h"
#include "SdcCodeSelection.h"

namespace clang::tidy::sdc {
class SdcNoConstantUnsignedWrapCheck final : public SdcPolicyCheck {
public:
    SdcNoConstantUnsignedWrapCheck(StringRef Name, ClangTidyContext *Context)
        : SdcPolicyCheck(Name, Context) {}
    void registerPPCallbacks(const SourceManager &, Preprocessor *, Preprocessor *) override;
    void registerMatchers(ast_matchers::MatchFinder *Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
private:
    AnalysisInstanceTracker Instances;
};
} // namespace clang::tidy::sdc
