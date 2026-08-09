#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

// Requires #undef to follow a macro definition in the same file.
class SdcUndefSameFileCheck : public ClangTidyCheck {
public:
    SdcUndefSameFileCheck(StringRef Name, ClangTidyContext* Context);
    void registerPPCallbacks(const SourceManager& SM, Preprocessor* PP,
                             Preprocessor* ModuleExpanderPP) override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
