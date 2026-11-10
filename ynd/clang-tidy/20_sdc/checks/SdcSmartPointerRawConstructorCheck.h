#pragma once
#include "SdcPolicyDiagnostic.h"

namespace clang::tidy::sdc {
class SdcSmartPointerRawConstructorCheck final : public SdcPolicyCheck {
public:
    SdcSmartPointerRawConstructorCheck(StringRef Name, ClangTidyContext* Context)
        : SdcPolicyCheck(Name, Context) {}
    void registerMatchers(ast_matchers::MatchFinder*) override;
    void check(const ast_matchers::MatchFinder::MatchResult&) override;
private:
    AnalysisInstanceTracker Instances;
};
}
