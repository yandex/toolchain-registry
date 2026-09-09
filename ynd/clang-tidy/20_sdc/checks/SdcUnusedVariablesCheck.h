#pragma once
#include "bridge_header.h"
#include "SdcCodeSelection.h"
#include "llvm/ADT/SmallPtrSet.h"

namespace clang::tidy::sdc {
class SdcUnusedVariablesCheck final : public ClangTidyCheck {
public:
    SdcUnusedVariablesCheck(StringRef Name, ClangTidyContext *Context)
        : ClangTidyCheck(Name, Context) {}
    void registerMatchers(ast_matchers::MatchFinder *Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
private:
    AnalysisInstanceTracker Instances;
    bool CollectedVariableUses = false;
    llvm::SmallPtrSet<const VarDecl *, 32> UsedVariables;
};
} // namespace clang::tidy::sdc
