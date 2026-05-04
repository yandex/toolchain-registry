#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // argument to std::move shall be a non-const lvalue
            //
            // This check detects two violations:
            // 1. Calling std::move on a const-qualified lvalue (ineffective move)
            // 2. Calling std::move on a temporary/prvalue (redundant move)
            class SdcStdMoveNonConstLvalueCheck: public ClangTidyCheck {
            public:
                SdcStdMoveNonConstLvalueCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
