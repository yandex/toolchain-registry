#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

class SdcIfElseIfFinalElseCheck : public ClangTidyCheck {
public:
    SdcIfElseIfFinalElseCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
