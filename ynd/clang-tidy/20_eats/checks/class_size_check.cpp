#include "class_size_check.h"

#include <clang/AST/DeclCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>

#include <iterator>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

namespace {

// Special members are not counted: they are not part of the class behaviour.
bool isCountedMethod(const CXXMethodDecl& method) {
    return !isa<CXXConstructorDecl, CXXDestructorDecl>(method) && !method.isCopyAssignmentOperator() &&
           !method.isMoveAssignmentOperator() && !method.isDeleted();
}

unsigned countPublicMethods(const CXXRecordDecl& record) {
    unsigned count = 0;
    for (const Decl* decl : record.decls()) {
        // getAsFunction() also unwraps member function templates.
        const auto* method = dyn_cast_or_null<CXXMethodDecl>(decl->getAsFunction());
        if (method && !decl->isImplicit() && decl->getAccess() == AS_public && isCountedMethod(*method)) {
            ++count;
        }
    }
    return count;
}

}  // namespace

void ClassSizeCheck::storeOptions(ClangTidyOptions::OptionMap& options) {
    Options.store(options, "MaxFields", maxFields_);
    Options.store(options, "MaxPublicMethods", maxPublicMethods_);
    Options.store(options, "IgnoreAggregates", ignoreAggregates_);
}

void ClassSizeCheck::registerMatchers(MatchFinder* finder) {
    finder->addMatcher(
        cxxRecordDecl(
            isDefinition(),
            unless(ast_matchers::isTemplateInstantiation()),
            unless(isLambda()),
            unless(isUnion()))
            .bind("class"),
        this);
}

void ClassSizeCheck::check(const MatchFinder::MatchResult& result) {
    const auto* record = result.Nodes.getNodeAs<CXXRecordDecl>("class");
    if (ignoreAggregates_ && record->isAggregate()) {
        return;
    }

    const auto fields = static_cast<unsigned>(std::distance(record->field_begin(), record->field_end()));
    if (fields > maxFields_) {
        diag(record->getLocation(), "class %0 has %1 fields, more than %2; split it into smaller classes")
            << record << fields << maxFields_;
    }
    const unsigned publicMethods = countPublicMethods(*record);
    if (publicMethods > maxPublicMethods_) {
        diag(record->getLocation(), "class %0 has %1 public methods, more than %2; split it into smaller classes")
            << record << publicMethods << maxPublicMethods_;
    }
}

}  // namespace clang::tidy::eats
