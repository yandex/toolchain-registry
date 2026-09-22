#pragma once
#include "SdcPolicyDiagnostic.h"
#include "SdcCodeSelection.h"

namespace clang::tidy::sdc {
class SdcInvariantConditionCheck final : public SdcPolicyCheck {
public:
    SdcInvariantConditionCheck(StringRef Name, ClangTidyContext *Context)
        : SdcPolicyCheck(Name, Context) {}
    void registerMatchers(ast_matchers::MatchFinder *Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
private:
    AnalysisInstanceTracker Instances;
};
} // namespace clang::tidy::sdc
