#pragma once

#include "SdcBannedHeaderFacilitiesCheck.h"

namespace clang {
namespace tidy {
namespace sdc {

// Scope (per [csignal.syn] and the rule's amplification):
//   functions: signal, raise
//   type:      sig_atomic_t
//   macros:    SIG_DFL, SIG_ERR, SIG_IGN,
//              SIGABRT, SIGFPE, SIGILL, SIGINT, SIGSEGV, SIGTERM
//
// Exception (per the rule):
//   `signal(X, SIG_IGN)` is permitted (to disable a signal). The X and
//   SIG_IGN references inside that call are also permitted.
class SdcNoCsignalFacilitiesCheck: public SdcBannedHeaderFacilitiesCheck {
public:
    SdcNoCsignalFacilitiesCheck(StringRef Name, ClangTidyContext* Context);

protected:
    ArrayRef<StringRef> getProhibitedFunctions() const override;
    ArrayRef<StringRef> getProhibitedTypes() const override;
    ArrayRef<StringRef> getProhibitedMacros() const override;
    StringRef getHeaderName() const override;
    bool isExemptCall(const CallExpr* Call) const override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
