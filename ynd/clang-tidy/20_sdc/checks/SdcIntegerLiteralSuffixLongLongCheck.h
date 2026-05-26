#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

// An integer-literal of type long long shall not use a single L or l in any
// suffix. Applies to both signed and unsigned long long literals.
//
// A literal with a suffix containing a single L could be a long long or an
// unsigned long long (depending on platform widths and the integer-rank
// ladder); LL/ll makes the long-long-ness explicit.
//
// Compliant:   12345678998ull, 0xfeeddeadbeefLL, 42L (type is long), 42 (no suffix)
// Violation:   12345678998L, 12345678998UL, 0xfeeddeadbeefL
class SdcIntegerLiteralSuffixLongLongCheck: public ClangTidyCheck {
public:
    SdcIntegerLiteralSuffixLongLongCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
