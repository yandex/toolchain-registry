#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Combined check for two related rules:
            //  - No implicit or explicit conversion FROM bool, except
            //    constructor-conversion to a class with a bool-taking
            //    constructor, and assignment to a 1-bit bit-field.
            //  - No implicit or explicit conversion TO bool, except
            //    contextual conversion from pointer or from a class with an
            //    explicit `operator bool`, conversion from a 1-bit
            //    bit-field, and the declarator-as-while-condition idiom.
            class SdcNoBoolConversionCheck: public ClangTidyCheck {
            public:
                SdcNoBoolConversionCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
