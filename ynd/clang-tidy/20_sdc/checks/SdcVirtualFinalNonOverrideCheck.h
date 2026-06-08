#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

// Companion to modernize-use-override: flags 'virtual final' on a member
// function that does not override anything in a base class.
// modernize-use-override handles all the other violations of this rule.
class SdcVirtualFinalNonOverrideCheck : public ClangTidyCheck {
public:
    SdcVirtualFinalNonOverrideCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
