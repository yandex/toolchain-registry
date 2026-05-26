#pragma once

#include "SdcBannedHeaderFacilitiesCheck.h"

namespace clang {
namespace tidy {
namespace sdc {

// Scope (per [cstdarg.syn]):
//   type:   va_list
//   macros: va_start, va_end, va_arg, va_copy
//
// Note (from the rule): this does not restrict the use of existing variadic
// functions or the declaration of functions that use the ellipsis. We
// satisfy that naturally - we match the named facilities, not the `...`
// syntax in declarations or calls to variadic functions like printf.
class SdcNoCstdargFacilitiesCheck: public SdcBannedHeaderFacilitiesCheck {
public:
    SdcNoCstdargFacilitiesCheck(StringRef Name, ClangTidyContext* Context);

protected:
    ArrayRef<StringRef> getProhibitedFunctions() const override;
    ArrayRef<StringRef> getProhibitedTypes() const override;
    ArrayRef<StringRef> getProhibitedMacros() const override;
    StringRef getHeaderName() const override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
