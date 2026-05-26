#pragma once

#include "SdcProhibitedFunctionsCheck.h"

namespace clang {
namespace tidy {
namespace sdc {

// Scope: the I/O functions specified by <cstdio> ([cstdio.syn]) and the
// wide-character I/O subset of <cwchar> ([cwchar.syn]). Per the rule's
// usual amplification, <stdio.h> and <wchar.h> are covered too via the
// global-namespace spellings.
//
// Wide-character string manipulation (wcscpy/wcscat/...), multibyte
// conversion (mbrtowc/...), and numeric conversion (wcstol/...) from
// <cwchar> are NOT in scope here - those are covered by sibling rules.
class SdcNoIoFunctionsCheck: public SdcProhibitedFunctionsCheck {
public:
    SdcNoIoFunctionsCheck(StringRef Name, ClangTidyContext* Context);

protected:
    ArrayRef<StringRef> getProhibitedFunctions() const override;
    std::string getDiagnosticMessage(StringRef FunctionName) const override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
