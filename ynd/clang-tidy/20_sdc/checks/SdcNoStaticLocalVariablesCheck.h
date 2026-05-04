#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Local variables shall not have static storage duration
            // This rule does not apply to variables declared constexpr or const.
            class SdcNoStaticLocalVariablesCheck: public ClangTidyCheck {
            public:
                SdcNoStaticLocalVariablesCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

            private:
                bool isLocalVariable(const clang::VarDecl* VarDecl,
                                     const ast_matchers::MatchFinder::MatchResult& Result);
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
