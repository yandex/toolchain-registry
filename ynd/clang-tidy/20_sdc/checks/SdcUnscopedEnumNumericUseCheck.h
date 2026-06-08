#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

// Enforces restrictions on numeric use of unscoped enumerations that have no
// fixed underlying type (i.e. no explicit enum-base).
class SdcUnscopedEnumNumericUseCheck : public ClangTidyCheck {
public:
    SdcUnscopedEnumNumericUseCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
