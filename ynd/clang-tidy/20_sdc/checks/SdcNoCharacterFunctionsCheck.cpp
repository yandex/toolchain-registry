#include "SdcNoCharacterFunctionsCheck.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Type.h"

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

bool SdcNoCharacterFunctionsCheck::isAllowedDecl(const FunctionDecl* FD) const {
    if (FD->getNumParams() != 2)
        return false;
    QualType SecondParam = FD->getParamDecl(1)->getType();
    const auto* RefTy = SecondParam->getAs<ReferenceType>();
    if (!RefTy)
        return false;
    QualType Pointee = RefTy->getPointeeType();
    if (!Pointee.isConstQualified())
        return false;
    const auto* RecordTy = Pointee->getAs<RecordType>();
    if (!RecordTy)
        return false;
    const RecordDecl* RD = RecordTy->getDecl();
    if (RD->getName() != "locale")
        return false;
    // libc++ declares locale in an inline ABI namespace beneath std.
    // Ordinary nested namespaces and user namespaces named std are not std.
    const DeclContext* DC = RD->getDeclContext();
    while (DC->isInlineNamespace())
        DC = DC->getParent();
    const auto* NS = dyn_cast<NamespaceDecl>(DC);
    return NS && NS->isStdNamespace();
}

} // namespace sdc
} // namespace tidy
} // namespace clang
