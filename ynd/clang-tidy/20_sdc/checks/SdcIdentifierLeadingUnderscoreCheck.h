#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

// identifier rule 5: an identifier that is not used as a literal
// suffix shall not start with `_`.
// template specializations are exempt because they do not introduce new identifiers.
class SdcIdentifierLeadingUnderscoreCheck : public ClangTidyCheck {
public:
    SdcIdentifierLeadingUnderscoreCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
