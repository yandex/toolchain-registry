#pragma once

#include "SdcProhibitedFunctionsCheck.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // library function `system` from <cstdlib> shall not be used
            class SdcNoSystemCheck: public SdcProhibitedFunctionsCheck {
            public:
                SdcNoSystemCheck(StringRef Name, ClangTidyContext* Context);

                void registerPPCallbacks(const SourceManager& SM,
                                         Preprocessor* PP,
                                         Preprocessor* ModuleExpanderPP) override;

            protected:
                ArrayRef<StringRef> getProhibitedFunctions() const override;
                std::string getDiagnosticMessage(StringRef FunctionName) const override;
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
