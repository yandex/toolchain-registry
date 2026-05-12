#include "SdcNoAtoFunctionsCheck.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            static const StringRef ProhibitedAtoFunctions[] = {
                "::atof", "::atoi", "::atol", "::atoll",
                "::std::atof", "::std::atoi", "::std::atol", "::std::atoll"
            };

            SdcNoAtoFunctionsCheck::SdcNoAtoFunctionsCheck(
                StringRef Name, ClangTidyContext* Context)
                : SdcProhibitedFunctionsCheck(Name, Context)
            {
            }

            ArrayRef<StringRef> SdcNoAtoFunctionsCheck::getProhibitedFunctions() const {
                return ProhibitedAtoFunctions;
            }

            std::string SdcNoAtoFunctionsCheck::getDiagnosticMessage(StringRef FunctionName) const {
                std::string Suggestion;
                if (FunctionName == "atoi") {
                    Suggestion = "; consider using std::stoi with proper error handling";
                } else if (FunctionName == "atol") {
                    Suggestion = "; consider using std::stol with proper error handling";
                } else if (FunctionName == "atof") {
                    Suggestion = "; consider using std::stod with proper error handling";
                } else if (FunctionName == "atoll") {
                    Suggestion = "; consider using std::stoll with proper error handling";
                }

                return "library function '" + FunctionName.str() + "' from <cstdlib> shall not be used" + Suggestion;
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
