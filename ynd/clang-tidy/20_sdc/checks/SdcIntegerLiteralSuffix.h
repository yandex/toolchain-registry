#pragma once

#include "clang/AST/Expr.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"

namespace clang {
namespace tidy {
namespace sdc {

// Parsed shape of an integer-literal's suffix as actually spelled in source.
struct ParsedIntegerLiteralSuffix {
    enum class Base { Decimal, Hex, Octal, Binary };

    Base base = Base::Decimal;
    bool hasU = false;  // any u/U present in the suffix
    int lCount = 0;     // count of l/L characters in the suffix (0, 1, or 2)
    bool valid = false; // false on parse failure or on user-defined literals
};

// Examine the source spelling of an IntegerLiteral (using its spelling
// location, so macros are checked against what the macro body spelled) and
// return the suffix's character composition. Digit separators (`'`) are
// stripped before suffix detection. UDL spellings (suffix begins with `_`)
// return valid==false: the suffix rules explicitly do not apply to
// user-defined-integer-literals.
ParsedIntegerLiteralSuffix parseIntegerLiteralSuffix(
    const IntegerLiteral* Literal,
    const SourceManager& SM,
    const LangOptions& LO);

} // namespace sdc
} // namespace tidy
} // namespace clang
