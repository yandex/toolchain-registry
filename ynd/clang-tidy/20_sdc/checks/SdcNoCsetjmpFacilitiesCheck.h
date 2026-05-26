#pragma once

#include "SdcBannedHeaderFacilitiesCheck.h"

namespace clang {
namespace tidy {
namespace sdc {

// Scope (per [csetjmp.syn]):
//   function: longjmp     (also sigsetjmp/siglongjmp are POSIX, not standard)
//   type:     jmp_buf
//   macro:    setjmp      (per the C++ standard, setjmp is a macro)
//
// Note (from the rule): the rule also applies to <setjmp.h>. Same names, so
// the global-namespace spellings in the lists cover that.
class SdcNoCsetjmpFacilitiesCheck: public SdcBannedHeaderFacilitiesCheck {
public:
    SdcNoCsetjmpFacilitiesCheck(StringRef Name, ClangTidyContext* Context);

protected:
    ArrayRef<StringRef> getProhibitedFunctions() const override;
    ArrayRef<StringRef> getProhibitedTypes() const override;
    ArrayRef<StringRef> getProhibitedMacros() const override;
    StringRef getHeaderName() const override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
