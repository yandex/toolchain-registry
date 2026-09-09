#pragma once

#include "bridge_header.h"

namespace clang::tidy::sdc {

class SdcMainCatchAllCheck final : public ClangTidyCheck {
public:
    SdcMainCatchAllCheck(StringRef Name, ClangTidyContext *Context)
        : ClangTidyCheck(Name, Context) {}

    void registerMatchers(ast_matchers::MatchFinder *Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace clang::tidy::sdc
