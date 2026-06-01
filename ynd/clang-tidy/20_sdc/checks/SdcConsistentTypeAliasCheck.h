#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Ensures that every redeclaration of the same entity uses the
            // same typedef / using-alias spelling for its declared type and,
            // for functions, for the return type and each parameter type.
            class SdcConsistentTypeAliasCheck: public ClangTidyCheck {
            public:
                SdcConsistentTypeAliasCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
