#include "SdcNoMemFunctionsCheck.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            static const StringRef ProhibitedMemFunctions[] = {
                "::memcpy", "::memmove", "::memcmp",
                "::std::memcpy", "::std::memmove", "::std::memcmp"
            };

            SdcNoMemFunctionsCheck::SdcNoMemFunctionsCheck(
                StringRef Name, ClangTidyContext* Context)
                : SdcProhibitedFunctionsCheck(Name, Context)
            {
            }

            ArrayRef<StringRef> SdcNoMemFunctionsCheck::getProhibitedFunctions() const {
                return ProhibitedMemFunctions;
            }

            std::string SdcNoMemFunctionsCheck::getDiagnosticMessage(StringRef FunctionName) const {
                std::string Suggestion;
                if (FunctionName == "memcpy" || FunctionName == "memmove") {
                    Suggestion = "; consider using type-safe alternatives like std::copy, assignment operators, or manual loops";
                } else if (FunctionName == "memcmp") {
                    Suggestion = "; consider using type-safe alternatives like comparison operators, std::equal, or manual element-wise comparison";
                }

                return "The C++ Standard Library function '" + FunctionName.str() + "' shall not be used" + Suggestion;
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
