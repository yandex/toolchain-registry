#pragma once
#include "SdcPolicyDiagnostic.h"
#include "SdcCodeSelection.h"

namespace clang::tidy::sdc {
class SdcCopyMoveSignaturesCheck final : public SdcPolicyCheck {
public:
    SdcCopyMoveSignaturesCheck(StringRef Name, ClangTidyContext *Context)
        : SdcPolicyCheck(Name, Context) {}
    void registerMatchers(ast_matchers::MatchFinder *Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
private:
    AnalysisInstanceTracker Instances;
    std::set<const Decl *> Reported;
};
} // namespace clang::tidy::sdc
