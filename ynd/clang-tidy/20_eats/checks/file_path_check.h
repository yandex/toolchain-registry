#pragma once

#include "bridge_header.h"

#include <clang/Basic/SourceManager.h>
#include <llvm/Support/Regex.h>

namespace clang::tidy::eats {

    struct FilePathInfo {
        std::string root;
        std::string relative;
        std::string expectedNamespace;
    };

    class FilePathCheck: public ClangTidyCheck {
    public:
        FilePathCheck(StringRef name, ClangTidyContext* context_ptr);

        bool isLanguageVersionSupported(const LangOptions& langOpts) const override {
            return langOpts.CPlusPlus;
        }

        void storeOptions(ClangTidyOptions::OptionMap& options) override;
        std::optional<FilePathInfo> filePath(StringRef fileName) const;
        std::optional<FilePathInfo> filePath(const SourceManager& sourceManager, SourceLocation loc) const;

    private:
        const std::string sourceRootRegex_;
        const std::string excludedPathRegex_;
        llvm::Regex rootRegex_;
        llvm::Regex excludeRegex_;
        bool valid_;
    };

    std::string namedNamespace(const DeclContext* context_ptr);

} // namespace clang::tidy::eats
