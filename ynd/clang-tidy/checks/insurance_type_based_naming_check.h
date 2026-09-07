#pragma once

#include "bridge_header.h"

namespace clang::tidy::arcadia {

    /// Enforces type-based naming conventions used by Insurance uservices.
    class InsuranceTypeBasedNamingCheck: public ClangTidyCheck {
    public:
        InsuranceTypeBasedNamingCheck(StringRef name, ClangTidyContext* context)
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
