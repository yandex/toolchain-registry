#include "SdcNoCstdargFacilitiesCheck.h"

namespace clang {
namespace tidy {
namespace sdc {

namespace {

// va_list lives in std:: via <cstdarg> and in the global ns via <stdarg.h>;
// the rule's amplification also covers <stdarg.h>.
const StringRef ProhibitedCstdargTypes[] = {
    "::va_list",
    "::std::va_list",
};

// va_start/end/arg/copy are macros per the standard.
const StringRef ProhibitedCstdargMacros[] = {
    "va_start", "va_end", "va_arg", "va_copy",
};

} // namespace

SdcNoCstdargFacilitiesCheck::SdcNoCstdargFacilitiesCheck(
    StringRef Name, ClangTidyContext* Context)
    : SdcBannedHeaderFacilitiesCheck(Name, Context) {}

ArrayRef<StringRef> SdcNoCstdargFacilitiesCheck::getProhibitedFunctions() const {
    return {};
}

ArrayRef<StringRef> SdcNoCstdargFacilitiesCheck::getProhibitedTypes() const {
    return ProhibitedCstdargTypes;
}

ArrayRef<StringRef> SdcNoCstdargFacilitiesCheck::getProhibitedMacros() const {
    return ProhibitedCstdargMacros;
}

StringRef SdcNoCstdargFacilitiesCheck::getHeaderName() const {
    return "<cstdarg>";
}

} // namespace sdc
} // namespace tidy
} // namespace clang
