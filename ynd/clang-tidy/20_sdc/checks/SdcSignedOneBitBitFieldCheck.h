#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

// Prohibits one-bit named bit-fields with signed integer type.
class SdcSignedOneBitBitFieldCheck : public ClangTidyCheck {
public:
    SdcSignedOneBitBitFieldCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
