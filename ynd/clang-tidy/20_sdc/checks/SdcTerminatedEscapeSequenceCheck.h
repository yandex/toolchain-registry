#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

// Requires numeric and universal escape sequences to be terminated.
class SdcTerminatedEscapeSequenceCheck : public ClangTidyCheck {
public:
    SdcTerminatedEscapeSequenceCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

private:
    void checkLiteralToken(SourceLocation TokenLocation,
                           SourceLocation DiagnosticLocation,
                           const SourceManager& SM,
                           const LangOptions& LangOpts);
};

} // namespace sdc
} // namespace tidy
} // namespace clang
