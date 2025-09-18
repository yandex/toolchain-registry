#pragma once

#include "bridge_header.h"

namespace clang::tidy::arcadia {

    class UtilTStringUpperCaseMethodsCheck : public ClangTidyCheck {
    public:
        UtilTStringUpperCaseMethodsCheck(StringRef name, ClangTidyContext* context);

        bool isLanguageVersionSupported(const LangOptions& langOpts) const override {
            return langOpts.CPlusPlus;
        }

        void registerMatchers(ast_matchers::MatchFinder* finder) override;
        void check(const ast_matchers::MatchFinder::MatchResult& result) override;
    };

}  // namespace clang::tidy::arcadia
