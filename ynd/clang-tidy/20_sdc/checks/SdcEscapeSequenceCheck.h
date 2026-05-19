#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

// within character literals and non-raw string literals,
// `\` shall only be used to form a defined escape sequence or universal
// character name.
//
// Defined escape sequences: \n \t \v \b \r \f \a \\ \? \' \" \<octal>
// \x<hex>. Universal character names: \u<4hex> \U<8hex>. Anything else
// (e.g. the GCC extension \e, the unknown \q, malformed UCNs) is a
// violation.
class SdcEscapeSequenceCheck : public ClangTidyCheck {
public:
    SdcEscapeSequenceCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

private:
    void checkLiteralToken(SourceLocation Loc,
                           const SourceManager& SM,
                           const LangOptions& LO);
};

} // namespace sdc
} // namespace tidy
} // namespace clang
