#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Disallows array-to-pointer decay at function-call argument
            // positions (including constructor calls). String literals passed
            // to pointer-to-character parameters are exempted.
            class SdcNoArrayDecayInCallCheck: public ClangTidyCheck {
            public:
                SdcNoArrayDecayInCallCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
