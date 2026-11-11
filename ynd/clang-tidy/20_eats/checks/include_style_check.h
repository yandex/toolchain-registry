#pragma once

#include "file_path_check.h"

namespace clang::tidy::eats {

    class IncludeStyleCheck: public FilePathCheck {
    public:
        using FilePathCheck::FilePathCheck;

        void registerPPCallbacks(const SourceManager& sourceManager, Preprocessor* preprocessor_ptr,
                                 Preprocessor*) override;
    };

} // namespace clang::tidy::eats
