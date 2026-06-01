#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Enforces operand-typing constraints on bitwise, complement,
            // and shift operators:
            //  - both operands of `&`, `|`, `^` (and their compound forms)
            //    must be unsigned (pre-promotion);
            //  - the operand of `~` must be unsigned (pre-promotion);
            //  - the left operand of `<<` / `>>` must be unsigned, with a
            //    narrow exception for non-negative constants that, after the
            //    shift, do not reach or cross the sign bit;
            //  - the right operand of `<<` / `>>` must be either a
            //    non-constant unsigned expression or a constant in
            //    [0, sizeof(T)*CHAR_BIT - 1].
            class SdcBitwiseOperandsUnsignedCheck: public ClangTidyCheck {
            public:
                SdcBitwiseOperandsUnsignedCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
