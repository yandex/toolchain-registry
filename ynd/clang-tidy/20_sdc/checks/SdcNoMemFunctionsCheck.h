#pragma once

#include "SdcProhibitedFunctionsCheck.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // The C++ Standard Library functions memcpy, memmove and
            // memcmp shall not be used
            class SdcNoMemFunctionsCheck: public SdcProhibitedFunctionsCheck {
            public:
                SdcNoMemFunctionsCheck(StringRef Name, ClangTidyContext* Context);

            protected:
                ArrayRef<StringRef> getProhibitedFunctions() const override;
                std::string getDiagnosticMessage(StringRef FunctionName) const override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
