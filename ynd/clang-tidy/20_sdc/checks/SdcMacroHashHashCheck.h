#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

class SdcMacroHashHashCheck : public ClangTidyCheck {
public:
    SdcMacroHashHashCheck(StringRef Name, ClangTidyContext* Context);
    void registerPPCallbacks(const SourceManager& SM, Preprocessor* PP,
                              Preprocessor* ModuleExpanderPP) override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
