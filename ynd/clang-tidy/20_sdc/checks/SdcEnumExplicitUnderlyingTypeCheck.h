#pragma once

#include "SdcCodeSelection.h"
#include "SdcPolicyDiagnostic.h"

namespace clang {
namespace tidy {
namespace sdc {

class SdcEnumExplicitUnderlyingTypeCheck : public SdcPolicyCheck {
public:
    SdcEnumExplicitUnderlyingTypeCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

private:
    AnalysisInstanceTracker AnalysisInstances;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
