#pragma once
#include "SdcPolicyDiagnostic.h"
#include "SdcCodeSelection.h"

namespace clang::tidy::sdc {
class SdcSimpleForCheck final : public SdcPolicyCheck {
public:
    SdcSimpleForCheck(StringRef Name, ClangTidyContext *Context)
        : SdcPolicyCheck(Name, Context) {}
    void registerMatchers(ast_matchers::MatchFinder *Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
private:
    AnalysisInstanceTracker Instances;
};
} // namespace clang::tidy::sdc
