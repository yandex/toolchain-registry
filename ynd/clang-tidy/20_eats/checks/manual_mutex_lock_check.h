#pragma once

#include "bridge_header.h"

namespace clang::tidy::eats {

class ManualMutexLockCheck : public ClangTidyCheck {
public:
    ManualMutexLockCheck(StringRef name, ClangTidyContext* context)
        : ClangTidyCheck(name, context)
    {}

    bool isLanguageVersionSupported(const LangOptions& langOpts) const override {
        return langOpts.CPlusPlus;
    }

    std::optional<TraversalKind> getCheckTraversalKind() const override {
        return TK_IgnoreUnlessSpelledInSource;
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& result) override;
};

}  // namespace clang::tidy::eats
