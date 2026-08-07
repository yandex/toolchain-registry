#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

// Unlike readability-braces-around-statements,
// this also covers the substatement of a switch statement.
class SdcBracesAroundStatementsCheck : public ClangTidyCheck {
public:
    SdcBracesAroundStatementsCheck(StringRef Name, ClangTidyContext* Context);

    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
