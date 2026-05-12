#include "SdcNoStringFunctionsCheck.h"

namespace clang {
    namespace tidy {
        namespace sdc {
            static const StringRef ProhibitedStringFunctions[] = {
                "::strcat", "::std::strcat",
                "::strchr", "::std::strchr",
                "::strcmp", "::std::strcmp",
                "::strcoll", "::std::strcoll",
                "::strcpy", "::std::strcpy",
                "::strcspn", "::std::strcspn",
                "::strerror", "::std::strerror",
                "::strlen", "::std::strlen",
                "::strncat", "::std::strncat",
                "::strncmp", "::std::strncmp",
                "::strncpy", "::std::strncpy",
                "::strpbrk", "::std::strpbrk",
                "::strrchr", "::std::strrchr",
                "::strspn", "::std::strspn",
                "::strstr", "::std::strstr",
                "::strtok", "::std::strtok",
                "::strxfrm", "::std::strxfrm",
                "::strtod", "::std::strtod",
                "::strtof", "::std::strtof",
                "::strtol", "::std::strtol",
                "::strtold", "::std::strtold",
                "::strtoll", "::std::strtoll",
                "::strtoul", "::std::strtoul",
                "::strtoull", "::std::strtoull",
                "::fgetwc", "::std::fgetwc",
                "::fputwc", "::std::fputwc",
                "::wcstod", "::std::wcstod",
                "::wcstof", "::std::wcstof",
                "::wcstol", "::std::wcstol",
                "::wcstold", "::std::wcstold",
                "::wcstoll", "::std::wcstoll",
                "::wcstoul", "::std::wcstoul",
                "::wcstoull", "::std::wcstoull",
                "::strtoimax", "::std::strtoimax",
                "::strtoumax", "::std::strtoumax",
                "::wcstoimax", "::std::wcstoimax",
                "::wcstoumax", "::std::wcstoumax"};

            SdcNoStringFunctionsCheck::SdcNoStringFunctionsCheck(
                StringRef Name, ClangTidyContext* Context)
                : SdcProhibitedFunctionsCheck(Name, Context)
            {
            }

            ArrayRef<StringRef> SdcNoStringFunctionsCheck::getProhibitedFunctions() const {
                return ProhibitedStringFunctions;
            }

            std::string SdcNoStringFunctionsCheck::getDiagnosticMessage(StringRef FunctionName) const {
                std::string Suggestion;
                if (FunctionName.starts_with("strto") || FunctionName.starts_with("wcsto")) {
                    Suggestion = "; consider using std::sto* functions with proper error handling";
                } else if (FunctionName.starts_with("str") || FunctionName.starts_with("wc")) {
                    Suggestion = "; consider using std::string or std::wstring with their member functions";
                } else {
                    Suggestion = "; consider using C++ alternatives with proper safety mechanisms";
                }

                return "The string handling function '" + FunctionName.str() + "' shall not be used" + Suggestion;
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
