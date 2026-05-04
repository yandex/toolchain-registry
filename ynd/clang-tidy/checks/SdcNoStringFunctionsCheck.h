#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // The string handling functions from <cstring>, <cstdlib>,
            // <cwchar> and <cinttypes> shall not be used
            //
            // Prohibited functions:
            // - From <cstring>: strcat, strchr, strcmp, strcoll, strcpy, strcspn,
            //   strerror, strlen, strncat, strncmp, strncpy, strpbrk, strrchr, strspn,
            //   strstr, strtok, strxfrm
            // - From <cstdlib>: strtod, strtof, strtol, strtold, strtoll, strtoul,
            //   strtoull
            // - From <cwchar>: fgetwc, fputwc, wcstod, wcstof, wcstol, wcstold,
            //   wcstoll, wcstoul, wcstoull
            // - From <cinttypes>: strtoimax, strtoumax, wcstoimax, wcstoumax
            class SdcNoStringFunctionsCheck: public ClangTidyCheck {
            public:
                SdcNoStringFunctionsCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

            private:
                void reportViolation(const CallExpr* Call, StringRef FunctionName,
                                     const ast_matchers::MatchFinder::MatchResult& Result);
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
