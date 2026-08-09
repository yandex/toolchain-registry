#pragma once

#include "bridge_header.h"
#include "llvm/ADT/DenseSet.h"

namespace clang {
namespace tidy {
namespace sdc {

// Prohibits virtual base specifiers.
class SdcNoVirtualInheritanceCheck : public ClangTidyCheck {
public:
    SdcNoVirtualInheritanceCheck(StringRef Name, ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

private:
    llvm::DenseSet<unsigned> ReportedLocations;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
