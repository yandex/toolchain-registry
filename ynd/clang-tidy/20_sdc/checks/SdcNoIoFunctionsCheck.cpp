#include "SdcNoIoFunctionsCheck.h"

namespace clang {
namespace tidy {
namespace sdc {

namespace {

// Per C++20 [cstdio.syn] and [cwchar.syn] (I/O subset only).
// Each name is listed in both the global namespace (for <stdio.h>/<wchar.h>
// users) and ::std:: (for <cstdio>/<cwchar> users).
const StringRef ProhibitedIoFunctions[] = {
    // ----- <cstdio> -----
    // File operations
    "::remove",   "::std::remove",
    "::rename",   "::std::rename",
    "::tmpfile",  "::std::tmpfile",
    "::tmpnam",   "::std::tmpnam",
    // File access
    "::fclose",   "::std::fclose",
    "::fflush",   "::std::fflush",
    "::fopen",    "::std::fopen",
    "::freopen",  "::std::freopen",
    "::setbuf",   "::std::setbuf",
    "::setvbuf",  "::std::setvbuf",
    // Formatted byte I/O
    "::fprintf",  "::std::fprintf",
    "::fscanf",   "::std::fscanf",
    "::printf",   "::std::printf",
    "::scanf",    "::std::scanf",
    "::snprintf", "::std::snprintf",
    "::sprintf",  "::std::sprintf",
    "::sscanf",   "::std::sscanf",
    "::vfprintf", "::std::vfprintf",
    "::vfscanf",  "::std::vfscanf",
    "::vprintf",  "::std::vprintf",
    "::vscanf",   "::std::vscanf",
    "::vsnprintf","::std::vsnprintf",
    "::vsprintf", "::std::vsprintf",
    "::vsscanf",  "::std::vsscanf",
    // Character I/O
    "::fgetc",    "::std::fgetc",
    "::fgets",    "::std::fgets",
    "::fputc",    "::std::fputc",
    "::fputs",    "::std::fputs",
    "::getc",     "::std::getc",
    "::getchar",  "::std::getchar",
    "::putc",     "::std::putc",
    "::putchar",  "::std::putchar",
    "::puts",     "::std::puts",
    "::ungetc",   "::std::ungetc",
    // Direct I/O
    "::fread",    "::std::fread",
    "::fwrite",   "::std::fwrite",
    // File positioning
    "::fgetpos",  "::std::fgetpos",
    "::fseek",    "::std::fseek",
    "::fsetpos",  "::std::fsetpos",
    "::ftell",    "::std::ftell",
    "::rewind",   "::std::rewind",
    // Error handling
    "::clearerr", "::std::clearerr",
    "::feof",     "::std::feof",
    "::ferror",   "::std::ferror",
    "::perror",   "::std::perror",

    // ----- <cwchar> (wide-character I/O subset) -----
    // Formatted wide I/O
    "::fwprintf",  "::std::fwprintf",
    "::fwscanf",   "::std::fwscanf",
    "::swprintf",  "::std::swprintf",
    "::swscanf",   "::std::swscanf",
    "::vfwprintf", "::std::vfwprintf",
    "::vfwscanf",  "::std::vfwscanf",
    "::vswprintf", "::std::vswprintf",
    "::vswscanf",  "::std::vswscanf",
    "::vwprintf",  "::std::vwprintf",
    "::vwscanf",   "::std::vwscanf",
    "::wprintf",   "::std::wprintf",
    "::wscanf",    "::std::wscanf",
    // Wide character I/O
    "::fgetwc",    "::std::fgetwc",
    "::fgetws",    "::std::fgetws",
    "::fputwc",    "::std::fputwc",
    "::fputws",    "::std::fputws",
    "::getwchar",  "::std::getwchar",
    "::putwchar",  "::std::putwchar",
    "::ungetwc",   "::std::ungetwc",
    // Stream orientation
    "::fwide",     "::std::fwide",
};

} // namespace

SdcNoIoFunctionsCheck::SdcNoIoFunctionsCheck(
    StringRef Name, ClangTidyContext* Context)
    : SdcProhibitedFunctionsCheck(Name, Context) {}

ArrayRef<StringRef> SdcNoIoFunctionsCheck::getProhibitedFunctions() const {
    return ProhibitedIoFunctions;
}

std::string SdcNoIoFunctionsCheck::getDiagnosticMessage(StringRef FunctionName) const {
    return ("C library input/output function '" + FunctionName +
            "' shall not be used").str();
}

} // namespace sdc
} // namespace tidy
} // namespace clang
