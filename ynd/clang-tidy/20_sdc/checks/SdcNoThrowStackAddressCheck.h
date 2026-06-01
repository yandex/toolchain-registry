#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Diagnoses `throw &local;` and equivalent forms, where the thrown
            // value is the address of an automatic-storage local variable or a
            // by-value function parameter. Complements clang's stock
            // -Wreturn-stack-address, which does not consider throws.
            class SdcNoThrowStackAddressCheck: public ClangTidyCheck {
            public:
                SdcNoThrowStackAddressCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
