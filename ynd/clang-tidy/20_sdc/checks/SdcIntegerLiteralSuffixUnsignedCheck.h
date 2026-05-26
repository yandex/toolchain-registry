#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

// Unsigned integer literals shall be appropriately suffixed.
//
// If an integer-literal's type as determined by [lex.icon] is unsigned, the
// literal must carry a u or U in its suffix. Examples (32-bit int):
//   0xFFFFFFFFu - compliant (explicit unsigned)
//   0xFFFFFFFF  - non-compliant (type is unsigned int via the integer-rank
//                 ladder, but the suffix doesn't say so)
// User-defined-integer-literals are excluded by the rule and by the AST node
// type (UserDefinedLiteral, not matched here).
class SdcIntegerLiteralSuffixUnsignedCheck: public ClangTidyCheck {
public:
    SdcIntegerLiteralSuffixUnsignedCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
