#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // The library functions atof, atoi, atol and atoll
            // from <cstdlib> shall not be used
            class SdcNoAtoFunctionsCheck: public SdcProhibitedFunctionsCheck {
            public:
                SdcNoAtoFunctionsCheck(StringRef Name, ClangTidyContext* Context);

            protected:
                ArrayRef<StringRef> getProhibitedFunctions() const override;
                std::string getDiagnosticMessage(StringRef FunctionName) const override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
