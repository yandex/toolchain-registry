#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

// forbids user-introduced identifiers that match a
// closed set of names reserved by the standard library (or by language
// extensions / contextual keywords).
class SdcBannedIdentifierNameCheck : public ClangTidyCheck {
public:
    SdcBannedIdentifierNameCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
