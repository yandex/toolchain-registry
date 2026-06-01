#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Diagnoses assignments of the form `lhs = &local`, `lhs = arr`,
            // or `lhs = std::addressof(local)` where the right-hand side
            // designates an automatic-storage variable whose lifetime is
            // strictly shorter than the left-hand side's. Two automatics
            // declared in the same block are treated as having the same
            // lifetime (per the project's decidable-subset interpretation of
            // the rule).
            class SdcNoStackAddressAssignCheck: public ClangTidyCheck {
            public:
                SdcNoStackAddressAssignCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
