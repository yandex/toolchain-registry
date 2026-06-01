#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Disallows implicit or explicit conversions that take an
            // expression of character category (char, wchar_t, char8_t,
            // char16_t, char32_t) to or from any other type, including
            // conversions between *different* character types. Exempts
            // unevaluated operands and equality-operator comparisons where
            // both operands have the same character type.
            class SdcNoCharacterConversionCheck: public ClangTidyCheck {
            public:
                SdcNoCharacterConversionCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
