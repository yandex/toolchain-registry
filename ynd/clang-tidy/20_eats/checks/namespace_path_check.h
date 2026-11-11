#pragma once

#include "file_path_check.h"

namespace clang::tidy::eats {

    class NamespacePathCheck: public FilePathCheck {
    public:
        using FilePathCheck::FilePathCheck;

        std::optional<TraversalKind> getCheckTraversalKind() const override {
            return TK_IgnoreUnlessSpelledInSource;
        }

        void registerMatchers(ast_matchers::MatchFinder* finder_ptr) override;
        void check(const ast_matchers::MatchFinder::MatchResult& result) override;
    };

} // namespace clang::tidy::eats
