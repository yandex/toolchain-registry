#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Diagnoses operands whose final type after integral promotion or
            // the usual arithmetic conversions differs in signedness or in
            // type category (bool / character / signed / unsigned /
            // floating) from the operand's pre-promotion source type.
            // Operands of increment / decrement are skipped per the rule's
            // explicit exemption. Compile-time constants are exempted when
            // converted from non-negative signed to unsigned, or from any
            // integral type to floating.
            class SdcNoArithmeticCategoryChangeCheck: public ClangTidyCheck {
            public:
                SdcNoArithmeticCategoryChangeCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
