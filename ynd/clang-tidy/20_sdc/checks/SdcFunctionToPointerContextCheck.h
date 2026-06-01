#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Diagnoses implicit function-to-pointer-to-function conversions
            // that occur outside the two contexts where the rule allows
            // them: as the operand of a static_cast, or being stored into an
            // object of pointer-to-function type (variable initialization,
            // assignment, function argument, return value, constructor
            // argument).
            class SdcFunctionToPointerContextCheck: public ClangTidyCheck {
            public:
                SdcFunctionToPointerContextCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
