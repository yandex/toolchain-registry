#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

// Prohibits an accessible base class from appearing through both virtual and
// non-virtual derivations in the same hierarchy.
class SdcVirtualAndNonVirtualBaseCheck : public ClangTidyCheck {
public:
    SdcVirtualAndNonVirtualBaseCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
