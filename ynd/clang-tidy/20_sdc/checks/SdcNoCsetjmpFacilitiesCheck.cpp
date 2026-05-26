#include "SdcNoCsetjmpFacilitiesCheck.h"

namespace clang {
namespace tidy {
namespace sdc {

namespace {

const StringRef ProhibitedCsetjmpFunctions[] = {
    "::longjmp",
    "::std::longjmp",
};

const StringRef ProhibitedCsetjmpTypes[] = {
    "::jmp_buf",
    "::std::jmp_buf",
};

// Per [csetjmp.syn], setjmp is a macro. Some implementations also expose it
// as a function; if so it shows up under a different (implementation-private)
// name like `_setjmp` after macro expansion, which we don't care about - the
// rule names `setjmp`, and the macro expansion at the user-source site is
// what we catch.
const StringRef ProhibitedCsetjmpMacros[] = {
    "setjmp",
};

} // namespace

SdcNoCsetjmpFacilitiesCheck::SdcNoCsetjmpFacilitiesCheck(
    StringRef Name, ClangTidyContext* Context)
    : SdcBannedHeaderFacilitiesCheck(Name, Context) {}

ArrayRef<StringRef> SdcNoCsetjmpFacilitiesCheck::getProhibitedFunctions() const {
    return ProhibitedCsetjmpFunctions;
}

ArrayRef<StringRef> SdcNoCsetjmpFacilitiesCheck::getProhibitedTypes() const {
    return ProhibitedCsetjmpTypes;
}

ArrayRef<StringRef> SdcNoCsetjmpFacilitiesCheck::getProhibitedMacros() const {
    return ProhibitedCsetjmpMacros;
}

StringRef SdcNoCsetjmpFacilitiesCheck::getHeaderName() const {
    return "<csetjmp>";
}

} // namespace sdc
} // namespace tidy
} // namespace clang
