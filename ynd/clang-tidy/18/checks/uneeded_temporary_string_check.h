#pragma once

#include "bridge_header.h"

namespace clang::tidy::arcadia {
    /// Finds temporary strings created from a literal / view / char pointer / ... where a string view is sufficient.
    class UnneededTemporaryStringCheck : public ClangTidyCheck {
    public:
        UnneededTemporaryStringCheck(StringRef name, ClangTidyContext* context);

        bool isLanguageVersionSupported(const LangOptions& langOpts) const override {
            return langOpts.CPlusPlus;
        }

        void registerMatchers(ast_matchers::MatchFinder* finder) override;
        void check(const ast_matchers::MatchFinder::MatchResult& result) override;
        void storeOptions(ClangTidyOptions::OptionMap &Opts) override;

    private:
        void CheckImplicitCast(const ast_matchers::MatchFinder::MatchResult& result, const ImplicitCastExpr& cast);

    private:
        bool DiagnoseNonConvertingConstructors_;  // TString(), TString(ptr, size) - cannot provide fix-its, only diagnose for manual replacement
        bool DiagnoseMutatingConstructors_;  // TString(std::string&&), move-ctors - ctors may have side effects
        bool DiagnoseImplicitCopyMoveConstructors_;  // TString(TFsPath) - arg may not be directly convertible to string view
    };

}  // namespace clang::tidy::arcadia
