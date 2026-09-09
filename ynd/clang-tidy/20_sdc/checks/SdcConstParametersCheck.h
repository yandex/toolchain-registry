#pragma once
#include "bridge_header.h"
#include "SdcCodeSelection.h"

namespace clang::tidy::sdc {
class SdcConstParametersCheck final : public ClangTidyCheck {
public:
    SdcConstParametersCheck(StringRef Name, ClangTidyContext *Context)
        : ClangTidyCheck(Name, Context) {}
    void registerMatchers(ast_matchers::MatchFinder *Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
private:
    AnalysisInstanceTracker Instances;
};
} // namespace clang::tidy::sdc
