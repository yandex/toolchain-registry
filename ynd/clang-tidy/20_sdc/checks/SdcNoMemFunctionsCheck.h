#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // The C++ Standard Library functions memcpy, memmove and
            // memcmp shall not be used
            class SdcNoMemFunctionsCheck: public ClangTidyCheck {
            public:
                SdcNoMemFunctionsCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

            private:
                void reportViolation(const CallExpr* Call, StringRef FunctionName,
                                     const ast_matchers::MatchFinder::MatchResult& Result);
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
