#pragma once

#include "SdcCodeSelection.h"
#include "SdcPolicyDiagnostic.h"

namespace clang {
namespace tidy {
namespace sdc {

// Requires numeric and universal escape sequences to be terminated.
class SdcTerminatedEscapeSequenceCheck : public SdcPolicyCheck {
public:
    SdcTerminatedEscapeSequenceCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

private:
    void checkLiteralToken(const DynTypedNode& Node,
                           SourceLocation TokenLocation,
                           SourceLocation DiagnosticLocation,
                           ASTContext& Context);
    AnalysisInstanceTracker AnalysisInstances;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
