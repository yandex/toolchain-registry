#pragma once

#include "bridge_header.h"

namespace clang::tidy::eats {

    class ComplexConditionCheck: public ClangTidyCheck {
    public:
        ComplexConditionCheck(StringRef name, ClangTidyContext* context_ptr)
            : ClangTidyCheck(name, context_ptr)
            , maxOperands_(Options.get("MaxOperands", 2U))
        {
        }

        bool isLanguageVersionSupported(const LangOptions& langOpts) const override {
            return langOpts.CPlusPlus;
        }

        std::optional<TraversalKind> getCheckTraversalKind() const override {
            return TK_IgnoreUnlessSpelledInSource;
        }

        void storeOptions(ClangTidyOptions::OptionMap& options) override;
        void registerMatchers(ast_matchers::MatchFinder* finder_ptr) override;
        void check(const ast_matchers::MatchFinder::MatchResult& result) override;

    private:
        const unsigned maxOperands_;
    };

} // namespace clang::tidy::eats
