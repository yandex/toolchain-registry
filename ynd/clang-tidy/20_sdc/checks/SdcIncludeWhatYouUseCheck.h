#pragma once

#include "bridge_header.h"

#include <memory>

namespace clang::tidy::sdc {

    // Require a declaration provider for each explicitly written type name.
    class SdcIncludeWhatYouUseCheck: public ClangTidyCheck {
    public:
        SdcIncludeWhatYouUseCheck(StringRef Name, ClangTidyContext* Context);
        ~SdcIncludeWhatYouUseCheck() override;

        bool isLanguageVersionSupported(const LangOptions& LangOpts) const override;
        void registerPPCallbacks(const SourceManager& SM, Preprocessor* PP,
                                 Preprocessor* ModuleExpanderPP) override;
        void registerMatchers(ast_matchers::MatchFinder* Finder) override;
        void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
        void onEndOfTranslationUnit() override;
        void storeOptions(ClangTidyOptions::OptionMap& Options) override;

    private:
        class Analysis;
        std::unique_ptr<Analysis> State;
    };

} // namespace clang::tidy::sdc
