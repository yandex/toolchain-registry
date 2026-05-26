#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

// String literals with different encoding prefixes shall not be concatenated.
//
// Encoding prefixes (per the rule): L (wide), u8 (UTF-8), u (char16_t),
// U (char32_t). An empty encoding-prefix is considered different from any
// non-empty one. The R prefix is NOT an encoding prefix and is ignored.
//
// Adjacent string literals are merged in translation phase 6, producing one
// AST StringLiteral with multiple source tokens accessible via
// getStrTokenLoc. This check inspects each token's spelling, extracts its
// encoding prefix, and reports any token whose prefix differs from the
// first token's prefix.
class SdcStringLiteralConcatenationCheck: public ClangTidyCheck {
public:
    SdcStringLiteralConcatenationCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
