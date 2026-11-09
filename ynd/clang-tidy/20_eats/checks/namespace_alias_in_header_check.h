#pragma once

#include "bridge_header.h"

namespace clang::tidy::eats {

    class NamespaceAliasInHeaderCheck: public ClangTidyCheck {
    public:
        NamespaceAliasInHeaderCheck(StringRef name, ClangTidyContext* context_ptr)
            : ClangTidyCheck(name, context_ptr)
            , headerFileExtensions_(context_ptr->getHeaderFileExtensions())
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

    private:
        const FileExtensionsSet headerFileExtensions_;
    };

} // namespace clang::tidy::eats
