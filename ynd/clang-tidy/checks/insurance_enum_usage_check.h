#pragma once

#include "bridge_header.h"

namespace clang::tidy::arcadia {

    /// Enforces the Insurance conventions for enum declarations and control flow.
    class InsuranceEnumUsageCheck: public ClangTidyCheck {
    public:
        InsuranceEnumUsageCheck(StringRef name, ClangTidyContext* context)
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
