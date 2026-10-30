#pragma once

#include "bridge_header.h"

#include <clang/Lex/Preprocessor.h>

#include <memory>

namespace clang::tidy::eats {

class TodoFormatCheck : public ClangTidyCheck {
public:
    TodoFormatCheck(StringRef name, ClangTidyContext* context);
    ~TodoFormatCheck() override;

    bool isLanguageVersionSupported(const LangOptions& langOpts) const override {
        return langOpts.CPlusPlus;
    }

    void registerPPCallbacks(const SourceManager& sourceManager, Preprocessor* preprocessor,
                             Preprocessor* moduleExpanderPreprocessor) override;

private:
    class TodoCommentHandler;

    std::unique_ptr<TodoCommentHandler> handler_;
};

}  // namespace clang::tidy::eats
