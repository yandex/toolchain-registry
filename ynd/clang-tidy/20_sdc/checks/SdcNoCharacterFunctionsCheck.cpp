#include "SdcNoCharacterFunctionsCheck.h"

namespace clang {
namespace tidy {
namespace sdc {

namespace {

// Per C++20 [cctype.syn] and [cwctype.syn].
const StringRef ProhibitedCharacterFunctions[] = {
    // ----- <cctype> -----
    "::isalnum",  "::std::isalnum",
    "::isalpha",  "::std::isalpha",
    "::isblank",  "::std::isblank",
    "::iscntrl",  "::std::iscntrl",
    "::isdigit",  "::std::isdigit",
    "::isgraph",  "::std::isgraph",
    "::islower",  "::std::islower",
    "::isprint",  "::std::isprint",
    "::ispunct",  "::std::ispunct",
    "::isspace",  "::std::isspace",
    "::isupper",  "::std::isupper",
    "::isxdigit", "::std::isxdigit",
    "::tolower",  "::std::tolower",
    "::toupper",  "::std::toupper",

    // ----- <cwctype> -----
    "::iswalnum",  "::std::iswalnum",
    "::iswalpha",  "::std::iswalpha",
    "::iswblank",  "::std::iswblank",
    "::iswcntrl",  "::std::iswcntrl",
    "::iswdigit",  "::std::iswdigit",
    "::iswgraph",  "::std::iswgraph",
    "::iswlower",  "::std::iswlower",
    "::iswprint",  "::std::iswprint",
    "::iswpunct",  "::std::iswpunct",
    "::iswspace",  "::std::iswspace",
    "::iswupper",  "::std::iswupper",
    "::iswxdigit", "::std::iswxdigit",
    "::iswctype",  "::std::iswctype",
    "::wctype",    "::std::wctype",
    "::towlower",  "::std::towlower",
    "::towupper",  "::std::towupper",
    "::towctrans", "::std::towctrans",
    "::wctrans",   "::std::wctrans",
};

} // namespace

SdcNoCharacterFunctionsCheck::SdcNoCharacterFunctionsCheck(
    StringRef Name, ClangTidyContext* Context)
    : SdcProhibitedFunctionsCheck(Name, Context) {}

ArrayRef<StringRef> SdcNoCharacterFunctionsCheck::getProhibitedFunctions() const {
    return ProhibitedCharacterFunctions;
}

std::string
SdcNoCharacterFunctionsCheck::getDiagnosticMessage(StringRef FunctionName) const {
    return ("character handling function '" + FunctionName +
            "' from <cctype>/<cwctype> shall not be used").str();
}

} // namespace sdc
} // namespace tidy
} // namespace clang
