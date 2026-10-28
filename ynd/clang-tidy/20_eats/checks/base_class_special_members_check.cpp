#include "base_class_special_members_check.h"

#include <clang/AST/DeclCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringExtras.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

void BaseClassSpecialMembersCheck::registerMatchers(MatchFinder* finder) {
    // isOverride() also covers a destructor whose base is dependent: it is not virtual until instantiation.
    finder->addMatcher(
        cxxRecordDecl(
            isDefinition(),
            unless(ast_matchers::isTemplateInstantiation()),
            unless(isFinal()),
            has(cxxDestructorDecl(unless(isImplicit()), anyOf(isVirtual(), isOverride())).bind("destructor")))
            .bind("class"),
        this);
}

void BaseClassSpecialMembersCheck::check(const MatchFinder::MatchResult& result) {
    const auto* record = result.Nodes.getNodeAs<CXXRecordDecl>("class");
    const auto* destructor = result.Nodes.getNodeAs<CXXDestructorDecl>("destructor");

    llvm::SmallVector<StringRef, 4> missing;
    if (!record->hasUserDeclaredCopyConstructor()) {
        missing.push_back("a copy constructor");
    }
    if (!record->hasUserDeclaredCopyAssignment()) {
        missing.push_back("a copy assignment operator");
    }
    if (!record->hasUserDeclaredMoveConstructor()) {
        missing.push_back("a move constructor");
    }
    if (!record->hasUserDeclaredMoveAssignment()) {
        missing.push_back("a move assignment operator");
    }
    if (missing.empty()) {
        return;
    }
    // A user-declared destructor suppresses the implicit move operations.
    diag(destructor->getLocation(), "class %0 declares a virtual destructor but not %1; "
                                    "declare copy and move operations explicitly, as '= default' or '= delete'")
        << record << llvm::join(missing, ", ");
}

}  // namespace clang::tidy::eats
