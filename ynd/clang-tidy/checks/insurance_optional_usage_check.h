#pragma once

#include "bridge_header.h"

namespace clang::tidy::arcadia {

    /// Enforces the Insurance conventions for accessing and checking std::optional.
    class InsuranceOptionalUsageCheck: public ClangTidyCheck {
    public:
        InsuranceOptionalUsageCheck(StringRef name, ClangTidyContext* context)
            : ClangTidyCheck(name, context)
        {
        }

        bool isLanguageVersionSupported(const LangOptions& langOpts) const override {
            return langOpts.CPlusPlus;
        }

        void registerMatchers(ast_matchers::MatchFinder* finder) override;
        void check(const ast_matchers::MatchFinder::MatchResult& result) override;
    };

} // namespace clang::tidy::arcadia
