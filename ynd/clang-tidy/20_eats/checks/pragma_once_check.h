#pragma once

#include "bridge_header.h"

namespace clang::tidy::eats {

class PragmaOnceCheck : public ClangTidyCheck {
public:
    PragmaOnceCheck(StringRef name, ClangTidyContext* context)
        : ClangTidyCheck(name, context)
        , headerFileExtensions_(context->getHeaderFileExtensions())
    {}

    bool isLanguageVersionSupported(const LangOptions& langOpts) const override {
        return langOpts.CPlusPlus;
    }

    void registerPPCallbacks(const SourceManager& sourceManager, Preprocessor* preprocessor,
                             Preprocessor* moduleExpanderPreprocessor) override;

private:
    const FileExtensionsSet headerFileExtensions_;
};

}  // namespace clang::tidy::eats
