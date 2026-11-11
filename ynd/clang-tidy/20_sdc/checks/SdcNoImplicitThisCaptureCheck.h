#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Diagnoses implicit this captures unless the callback is invoked
            // immediately or its consumers are proven not to retain it.
            class SdcNoImplicitThisCaptureCheck: public ClangTidyCheck {
            public:
                SdcNoImplicitThisCaptureCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
