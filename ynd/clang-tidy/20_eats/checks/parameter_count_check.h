#pragma once

#include "bridge_header.h"

namespace clang::tidy::eats {

class ParameterCountCheck : public ClangTidyCheck {
public:
    ParameterCountCheck(StringRef name, ClangTidyContext* context)
        : ClangTidyCheck(name, context)
        , parameterThreshold_(Options.get("ParameterThreshold", 4U))
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
    const unsigned parameterThreshold_;
};

}  // namespace clang::tidy::eats
