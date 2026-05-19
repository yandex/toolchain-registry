#include "SdcNoSystemCheck.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            static const StringRef ProhibitedSystemFunctions[] = {
                "::system",
                "::std::system",
                "::boost::process::system"
            };

            SdcNoSystemCheck::SdcNoSystemCheck(
                StringRef Name, ClangTidyContext* Context)
                : SdcProhibitedFunctionsCheck(Name, Context)
            {
            }

            ArrayRef<StringRef> SdcNoSystemCheck::getProhibitedFunctions() const {
                return ProhibitedSystemFunctions;
            }

            std::string SdcNoSystemCheck::getDiagnosticMessage(StringRef FunctionName) const {
                return "library function 'system' from <cstdlib> or its boost alternative shall not be used";
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
