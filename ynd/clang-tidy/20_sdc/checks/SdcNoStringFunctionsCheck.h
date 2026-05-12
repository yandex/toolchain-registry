#pragma once

#include "SdcProhibitedFunctionsCheck.h"

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
            class SdcNoStringFunctionsCheck: public SdcProhibitedFunctionsCheck {
            public:
                SdcNoStringFunctionsCheck(StringRef Name, ClangTidyContext* Context);

            protected:
                ArrayRef<StringRef> getProhibitedFunctions() const override;
                std::string getDiagnosticMessage(StringRef FunctionName) const override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
