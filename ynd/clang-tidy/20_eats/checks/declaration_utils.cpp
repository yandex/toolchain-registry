#include "declaration_utils.h"

#include <clang/AST/Attr.h>
#include <clang/AST/DeclCXX.h>

namespace clang::tidy::eats {

    bool isOwnParameterName(const ParmVarDecl& parameter) {
        // Function-type parameters, as in 'void (*callback)(int code)', name no variable.
        const auto* function_ptr = dyn_cast<FunctionDecl>(parameter.getDeclContext());
        if (!function_ptr || function_ptr->getCanonicalDecl() != function_ptr) {
            return false;
        }
        const auto* method_ptr = dyn_cast<CXXMethodDecl>(function_ptr);
        // Dependent bases are unresolved in a template pattern, but 'override' is already present.
        return !method_ptr ||
               (method_ptr->size_overridden_methods() == 0 && !method_ptr->hasAttr<OverrideAttr>());
    }

} // namespace clang::tidy::eats
