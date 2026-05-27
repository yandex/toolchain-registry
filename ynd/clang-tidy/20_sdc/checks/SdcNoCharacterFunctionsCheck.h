#pragma once

#include "SdcProhibitedFunctionsCheck.h"

namespace clang {
namespace tidy {
namespace sdc {

// Scope: every function specified by [cctype.syn] and [cwctype.syn]. The
// rule's usual amplification also covers <ctype.h> and <wctype.h> via the
// global-namespace spellings.
class SdcNoCharacterFunctionsCheck: public SdcProhibitedFunctionsCheck {
public:
    SdcNoCharacterFunctionsCheck(StringRef Name, ClangTidyContext* Context);

protected:
    ArrayRef<StringRef> getProhibitedFunctions() const override;
    std::string getDiagnosticMessage(StringRef FunctionName) const override;
    bool isAllowedDecl(const FunctionDecl* FD) const override;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
