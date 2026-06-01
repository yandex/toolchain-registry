#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Requires nullptr as the sole form of the null-pointer
            // constant, and forbids the macro NULL from appearing in any
            // context whatsoever.
            class SdcUseNullptrCheck: public ClangTidyCheck {
            public:
                SdcUseNullptrCheck(StringRef Name, ClangTidyContext* Context);
                void registerPPCallbacks(const SourceManager& SM, Preprocessor* PP,
                                         Preprocessor* ModuleExpanderPP) override;
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
