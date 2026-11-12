#pragma once

#include <clang/AST/Decl.h>

namespace clang::tidy::eats {

    // Overrides and redeclarations take parameter names from the first declaration.
    bool isOwnParameterName(const ParmVarDecl& parameter);

} // namespace clang::tidy::eats
