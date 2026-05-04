#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // The library functions atof, atoi, atol and atoll
            // from <cstdlib> shall not be used
            class SdcNoAtoFunctionsCheck: public ClangTidyCheck {
            public:
                SdcNoAtoFunctionsCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

            private:
                void reportViolation(const CallExpr* Call, StringRef FunctionName,
                                     const ast_matchers::MatchFinder::MatchResult& Result);
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
