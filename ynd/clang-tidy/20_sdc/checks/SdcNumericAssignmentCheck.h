#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Enforces appropriateness of assignments between numeric types
            // (signed integer, unsigned integer, floating). Covers
            // initialization, plain assignment, return-value initialization,
            // and function-call argument passing. Skips bool / character
            // types, which are governed by their own rules.
            class SdcNumericAssignmentCheck: public ClangTidyCheck {
            public:
                SdcNumericAssignmentCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
