#pragma once

#include "SdcBannedHeaderFacilitiesCheck.h"

namespace clang {
namespace tidy {
namespace sdc {

class SdcNoOffsetofCheck: public SdcBannedHeaderFacilitiesCheck {
public:
    SdcNoOffsetofCheck(StringRef Name, ClangTidyContext* Context);

protected:
    ArrayRef<StringRef> getProhibitedFunctions() const override;
    ArrayRef<StringRef> getProhibitedTypes() const override;
    ArrayRef<StringRef> getProhibitedMacros() const override;
    StringRef getHeaderName() const override;
    std::string getDiagnosticMessage(StringRef FacilityName) const override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
