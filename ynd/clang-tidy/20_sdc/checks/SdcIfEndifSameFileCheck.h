#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

class SdcIfEndifSameFileCheck : public ClangTidyCheck {
public:
    SdcIfEndifSameFileCheck(StringRef Name, ClangTidyContext* Context);
    void registerPPCallbacks(const SourceManager& SM, Preprocessor* PP,
                              Preprocessor* ModuleExpanderPP) override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
