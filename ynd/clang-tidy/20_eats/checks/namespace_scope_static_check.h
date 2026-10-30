#pragma once

#include "bridge_header.h"

namespace clang::tidy::eats {

class NamespaceScopeStaticCheck : public ClangTidyCheck {
public:
    NamespaceScopeStaticCheck(StringRef name, ClangTidyContext* context)
        : ClangTidyCheck(name, context)
        , headerFileExtensions_(context->getHeaderFileExtensions())
    {}

    bool isLanguageVersionSupported(const LangOptions& langOpts) const override {
        return langOpts.CPlusPlus;
    }

    std::optional<TraversalKind> getCheckTraversalKind() const override {
        return TK_IgnoreUnlessSpelledInSource;
    }

    void registerMatchers(ast_matchers::MatchFinder* finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& result) override;

private:
    const FileExtensionsSet headerFileExtensions_;
};

}  // namespace clang::tidy::eats
