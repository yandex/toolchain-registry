#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // Octal constants shall not be used.
            // The integer constant 0 (written as a single numeric digit) is an octal constant,
            // but its use is permitted as an exception to this rule.
            class SdcNoOctalConstantsCheck: public ClangTidyCheck {
            public:
                SdcNoOctalConstantsCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

            private:
                void checkIntegerLiteral(const clang::IntegerLiteral* Literal,
                                         const ast_matchers::MatchFinder::MatchResult& Result);
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
