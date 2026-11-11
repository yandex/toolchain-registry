#pragma once

#include "file_path_check.h"

#include <llvm/ADT/SmallVector.h>

#include <map>

namespace clang::tidy::eats {

    class ClassFileNameCheck: public FilePathCheck {
    public:
        ClassFileNameCheck(StringRef name, ClangTidyContext* context_ptr)
            : FilePathCheck(name, context_ptr)
            , helperMaxMethods_(Options.get("HelperMaxMethods", 3U))
            , helperMaxFields_(Options.get("HelperMaxFields", 3U))
        {
        }

        std::optional<TraversalKind> getCheckTraversalKind() const override {
            return TK_IgnoreUnlessSpelledInSource;
        }

        void storeOptions(ClangTidyOptions::OptionMap& options) override;
        void registerMatchers(ast_matchers::MatchFinder* finder_ptr) override;
        void check(const ast_matchers::MatchFinder::MatchResult& result) override;
        void onEndOfTranslationUnit() override;

    private:
        const unsigned helperMaxMethods_;
        const unsigned helperMaxFields_;
        std::map<std::string, llvm::SmallVector<const CXXRecordDecl*, 4>> classes_;
        const SourceManager* sourceManager_ptr_ = nullptr;
    };

} // namespace clang::tidy::eats
