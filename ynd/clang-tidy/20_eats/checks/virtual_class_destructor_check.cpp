#include "virtual_class_destructor_check.h"

#include <clang/AST/DeclCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <llvm/ADT/STLExtras.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

namespace {

AST_MATCHER(CXXRecordDecl, isPolymorphicClass) {
    return Node.isPolymorphic();
}

// getDestructor() returns only the selected destructor, and Sema selects one only in
// non-dependent classes; a class template pattern has no implicit destructor at all.
const CXXDestructorDecl* findDestructor(const CXXRecordDecl& record) {
    if (const CXXDestructorDecl* destructor = record.getDestructor()) {
        return destructor;
    }
    for (const CXXMethodDecl* method : record.methods()) {
        if (const auto* destructor = dyn_cast<CXXDestructorDecl>(method)) {
            return destructor;
        }
    }
    return nullptr;
}

// Without a declared destructor the implicit one is virtual only if a base destructor is.
bool hasVirtualDestructor(const CXXRecordDecl& record) {
    if (const CXXDestructorDecl* destructor = findDestructor(record)) {
        return destructor->isVirtual();
    }
    return llvm::any_of(record.bases(), [](const CXXBaseSpecifier& base) {
        const CXXRecordDecl* baseRecord = base.getType()->getAsCXXRecordDecl();
        return baseRecord && baseRecord->hasDefinition() && hasVirtualDestructor(*baseRecord->getDefinition());
    });
}

}  // namespace

void VirtualClassDestructorCheck::registerMatchers(MatchFinder* finder) {
    // An explicit instantiation is a separate record spelled in source: skip it to report the template once.
    finder->addMatcher(
        cxxRecordDecl(
            isDefinition(),
            unless(ast_matchers::isTemplateInstantiation()),
            isPolymorphicClass(),
            unless(isFinal()))
            .bind("class"),
        this);
}

void VirtualClassDestructorCheck::check(const MatchFinder::MatchResult& result) {
    const auto* record = result.Nodes.getNodeAs<CXXRecordDecl>("class");
    // A dependent base may make the destructor virtual on instantiation.
    if (record->hasAnyDependentBases() || hasVirtualDestructor(*record)) {
        return;
    }

    const CXXDestructorDecl* destructor = findDestructor(*record);
    if (!destructor || destructor->isImplicit()) {
        diag(record->getLocation(), "polymorphic class %0 has an implicit non-virtual destructor; "
                                    "declare a virtual destructor")
            << record;
        return;
    }
    // A protected non-virtual destructor forbids deletion through a pointer to this class.
    if (destructor->getAccess() != AS_public || destructor->isDeleted()) {
        return;
    }
    diag(destructor->getLocation(), "public destructor of polymorphic class %0 is not virtual; "
                                    "make it virtual or protected")
        << record;
}

}  // namespace clang::tidy::eats
