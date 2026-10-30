#pragma once

#include "bridge_header.h"

namespace clang::tidy::eats {

class LambdaSizeCheck : public ClangTidyCheck {
public:
    LambdaSizeCheck(StringRef name, ClangTidyContext* context)
        : ClangTidyCheck(name, context)
        , lineThreshold_(Options.get("LineThreshold", 15U))
    {}

    bool isLanguageVersionSupported(const LangOptions& langOpts) const override {
        return langOpts.CPlusPlus;
    }

    std::optional<TraversalKind> getCheckTraversalKind() const override {
        return TK_IgnoreUnlessSpelledInSource;
    }

    void storeOptions(ClangTidyOptions::OptionMap& options) override;

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& result) override;

private:
    const unsigned lineThreshold_;
};

}  // namespace clang::tidy::eats
