#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Diagnoses lambdas that capture `this` implicitly (via [=] or
            // [&]) unless the lambda is "transient" - here defined as a
            // lambda passed directly as a function-call argument, which is
            // consumed within that full-expression.
            class SdcNoImplicitThisCaptureCheck: public ClangTidyCheck {
            public:
                SdcNoImplicitThisCaptureCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
