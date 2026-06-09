#pragma once

#include "bridge_header.h"

namespace clang {
namespace tidy {
namespace sdc {

class SdcIncludeFilenameCharsCheck : public ClangTidyCheck {
public:
    SdcIncludeFilenameCharsCheck(StringRef Name, ClangTidyContext* Context);
    void registerPPCallbacks(const SourceManager& SM, Preprocessor* PP,
                              Preprocessor* ModuleExpanderPP) override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
