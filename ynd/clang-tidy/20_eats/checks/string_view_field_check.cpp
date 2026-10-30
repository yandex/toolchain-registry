#include "string_view_field_check.h"

#include <clang/AST/DeclTemplate.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

namespace {

bool argumentStoresStringView(const TemplateArgument& argument);

// Looks through template type arguments, so std::optional<std::string_view> counts,
// but not through function types: std::function<void(std::string_view)> stores no view.
bool storesStringView(QualType type) {
    const auto* specialization =
        dyn_cast_or_null<ClassTemplateSpecializationDecl>(type.getCanonicalType()->getAsCXXRecordDecl());
    if (!specialization) {
        return false;
    }
    if (specialization->isInStdNamespace() && specialization->getName() == "basic_string_view") {
        return true;
    }
    return llvm::any_of(specialization->getTemplateArgs().asArray(), argumentStoresStringView);
}

bool argumentStoresStringView(const TemplateArgument& argument) {
    switch (argument.getKind()) {
        case TemplateArgument::Type:
            return storesStringView(argument.getAsType());
        case TemplateArgument::Pack:
            return llvm::any_of(argument.pack_elements(), argumentStoresStringView);
        default:
            return false;
    }
}

AST_MATCHER(QualType, storesStringViewType) {
    return storesStringView(Node);
}

}  // namespace

void StringViewFieldCheck::registerMatchers(MatchFinder* finder) {
    finder->addMatcher(
        fieldDecl(
            hasType(storesStringViewType()),
            unless(isInstantiated()),
            unless(hasParent(cxxRecordDecl(isLambda()))))
            .bind("field"),
        this);
}

void StringViewFieldCheck::check(const MatchFinder::MatchResult& result) {
    const auto* field = result.Nodes.getNodeAs<FieldDecl>("field");
    diag(field->getLocation(), "field '%0' stores std::string_view that may outlive the referenced string; "
                               "store std::string")
        << field->getName();
}

}  // namespace clang::tidy::eats
