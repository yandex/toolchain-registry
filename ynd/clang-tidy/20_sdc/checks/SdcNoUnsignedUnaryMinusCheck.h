#pragma once

#include "bridge_header.h"
#include "llvm/ADT/DenseSet.h"

namespace clang {
namespace tidy {
namespace sdc {

// Prohibits applying built-in unary - to an unsigned expression.
class SdcNoUnsignedUnaryMinusCheck : public ClangTidyCheck {
public:
    SdcNoUnsignedUnaryMinusCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

private:
    llvm::DenseSet<unsigned> ReportedLocations;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
