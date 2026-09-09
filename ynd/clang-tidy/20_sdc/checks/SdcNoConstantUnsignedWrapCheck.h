#pragma once
#include "bridge_header.h"
#include "SdcCodeSelection.h"

namespace clang::tidy::sdc {
class SdcNoConstantUnsignedWrapCheck final : public ClangTidyCheck {
public:
    SdcNoConstantUnsignedWrapCheck(StringRef Name, ClangTidyContext *Context)
        : ClangTidyCheck(Name, Context) {}
    void registerPPCallbacks(const SourceManager &, Preprocessor *, Preprocessor *) override;
    void registerMatchers(ast_matchers::MatchFinder *Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
private:
    AnalysisInstanceTracker Instances;
};
} // namespace clang::tidy::sdc
