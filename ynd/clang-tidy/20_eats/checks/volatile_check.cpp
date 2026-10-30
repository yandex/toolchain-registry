#include "volatile_check.h"

#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

namespace {

bool containsVolatile(QualType type) {
    for (type = type.getCanonicalType(); !type.isNull();) {
        if (type.isVolatileQualified()) {
            return true;
        }
        if (const auto* array = dyn_cast<ArrayType>(type.getTypePtr())) {
            type = array->getElementType();
        } else if (type->isPointerType() || type->isReferenceType()) {
            type = type->getPointeeType();
        } else {
            return false;
        }
    }
    return false;
}

AST_MATCHER(QualType, containsVolatileQualifier) {
    return containsVolatile(Node);
}

}  // namespace

void VolatileCheck::registerMatchers(MatchFinder* finder) {
    finder->addMatcher(
        declaratorDecl(anyOf(varDecl(), fieldDecl()), unless(isInstantiated()), hasType(containsVolatileQualifier()))
            .bind("decl"),
        this);
}

void VolatileCheck::check(const MatchFinder::MatchResult& result) {
    const auto* decl = result.Nodes.getNodeAs<DeclaratorDecl>("decl");
    diag(decl->getLocation(), "do not use 'volatile' for '%0'; use std::atomic for inter-thread synchronization")
        << decl->getName();
}

}  // namespace clang::tidy::eats
