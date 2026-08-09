#pragma once

#include "bridge_header.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

namespace clang {
namespace tidy {
namespace sdc {

// Requires separate declarations for variables and member variables.
class SdcSingleVariableDeclarationCheck : public ClangTidyCheck {
public:
    SdcSingleVariableDeclarationCheck(StringRef Name,
                                      ClangTidyContext* Context);
    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

private:
    llvm::DenseMap<unsigned, llvm::DenseSet<unsigned>> GroupMembers;
    llvm::DenseSet<unsigned> ReportedGroups;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
