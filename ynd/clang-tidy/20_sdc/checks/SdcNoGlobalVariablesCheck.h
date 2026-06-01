#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Disallows variables with namespace scope and class static data
            // members unless they are constexpr, or const with constant
            // (static) initialization.
            class SdcNoGlobalVariablesCheck: public ClangTidyCheck {
            public:
                SdcNoGlobalVariablesCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
