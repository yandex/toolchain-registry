#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Diagnoses casts from a virtual base class to a derived class
            // that are not performed via dynamic_cast. Applies to both
            // pointer and reference casts.
            class SdcVirtualBaseCastViaDynamicCheck: public ClangTidyCheck {
            public:
                SdcVirtualBaseCastViaDynamicCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
