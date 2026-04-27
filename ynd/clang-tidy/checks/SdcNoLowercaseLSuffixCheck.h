#pragma once
#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {
            // lowercase form of `L` shall not be used as the first
            // character in a literal suffix.
            // This rule does not apply to user-defined-literals
            // as they start with `_`
            class SdcNoLowercaseLSuffixCheck: public ClangTidyCheck {
            public:
                SdcNoLowercaseLSuffixCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

            private:
                void checkIntegerLiteral(const clang::IntegerLiteral* Literal,
                                         const ast_matchers::MatchFinder::MatchResult& Result);
                void checkFloatingLiteral(const clang::FloatingLiteral* Literal,
                                          const ast_matchers::MatchFinder::MatchResult& Result);
                void checkStringLiteral(const clang::StringLiteral* Literal,
                                        const ast_matchers::MatchFinder::MatchResult& Result);
                void checkCharacterLiteral(const clang::CharacterLiteral* Literal,
                                           const ast_matchers::MatchFinder::MatchResult& Result);
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
