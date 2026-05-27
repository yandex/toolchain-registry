#include "SdcNoOffsetofCheck.h"

namespace clang {
namespace tidy {
namespace sdc {

namespace {

const StringRef ProhibitedMacros[] = {
    "offsetof",
};

} // namespace

SdcNoOffsetofCheck::SdcNoOffsetofCheck(
    StringRef Name, ClangTidyContext* Context)
    : SdcBannedHeaderFacilitiesCheck(Name, Context) {}

ArrayRef<StringRef> SdcNoOffsetofCheck::getProhibitedFunctions() const {
    return {};
}

ArrayRef<StringRef> SdcNoOffsetofCheck::getProhibitedTypes() const {
    return {};
}

ArrayRef<StringRef> SdcNoOffsetofCheck::getProhibitedMacros() const {
    return ProhibitedMacros;
}

StringRef SdcNoOffsetofCheck::getHeaderName() const {
    return "offsetof";
}

std::string SdcNoOffsetofCheck::getDiagnosticMessage(StringRef FacilityName) const {
    return "the offsetof macro shall not be used";
}

} // namespace sdc
} // namespace tidy
} // namespace clang
