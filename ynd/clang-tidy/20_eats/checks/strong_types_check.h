#pragma once

#include "bridge_header.h"

namespace clang::tidy::eats {

    class StrongTypesCheck: public ClangTidyCheck {
    public:
        StrongTypesCheck(StringRef name, ClangTidyContext* context_ptr)
            : ClangTidyCheck(name, context_ptr)
        {
        }

        bool isLanguageVersionSupported(const LangOptions& langOpts) const override {
            return langOpts.CPlusPlus;
        }

        std::optional<TraversalKind> getCheckTraversalKind() const override {
            return TK_IgnoreUnlessSpelledInSource;
        }

        void registerMatchers(ast_matchers::MatchFinder* finder_ptr) override;
        void check(const ast_matchers::MatchFinder::MatchResult& result) override;
    };

} // namespace clang::tidy::eats
