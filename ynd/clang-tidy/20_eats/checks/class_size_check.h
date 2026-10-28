#pragma once

#include "bridge_header.h"

namespace clang::tidy::eats {

class ClassSizeCheck : public ClangTidyCheck {
public:
    ClassSizeCheck(StringRef name, ClangTidyContext* context)
        : ClangTidyCheck(name, context)
        , maxFields_(Options.get("MaxFields", 7U))
        , maxPublicMethods_(Options.get("MaxPublicMethods", 10U))
        , ignoreAggregates_(Options.get("IgnoreAggregates", true))
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
    const unsigned maxFields_;
    const unsigned maxPublicMethods_;
    const bool ignoreAggregates_;
};

}  // namespace clang::tidy::eats
