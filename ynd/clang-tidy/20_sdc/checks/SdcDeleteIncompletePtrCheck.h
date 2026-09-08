#pragma once

#include "SdcCodeSelection.h"
#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

class SdcDeleteIncompletePtrCheck : public ClangTidyCheck {
public:
    SdcDeleteIncompletePtrCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

private:
    AnalysisInstanceTracker AnalysisInstances;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
