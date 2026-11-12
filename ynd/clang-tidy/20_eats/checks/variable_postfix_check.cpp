#include "variable_postfix_check.h"

#include "declaration_utils.h"

#include <clang/AST/DeclCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <llvm/ADT/STLExtras.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

    namespace {

        bool isStdClass(const CXXRecordDecl* record_ptr, std::initializer_list<StringRef> names) {
            return record_ptr && record_ptr->isInStdNamespace() && record_ptr->getIdentifier() &&
                   llvm::is_contained(names, record_ptr->getName());
        }

        // Container iterators use member typedefs; canonical types are implementation-specific.
        bool isIterator(QualType type) {
            for (QualType current = type;;) {
                if (const auto* typedef_ptr = dyn_cast<TypedefType>(current.getTypePtr())) {
                    if (typedef_ptr->getDecl()->getName().ends_with("iterator")) {
                        return true;
                    }
                }
                const QualType next = current->getLocallyUnqualifiedSingleStepDesugaredType();
                if (next == current.getLocalUnqualifiedType()) {
                    break;
                }
                current = next;
            }
            const auto* record_ptr = type.getCanonicalType()->getAsCXXRecordDecl();
            return record_ptr && record_ptr->getIdentifier() &&
                   record_ptr->getName().contains_insensitive("iterator");
        }

        std::optional<StringRef> requiredPostfix(QualType type) {
            if (type.isNull() || type->isDependentType()) {
                return std::nullopt;
            }
            type = type.getNonReferenceType();
            if (isIterator(type)) {
                return StringRef("_it");
            }
            const QualType canonical = type.getCanonicalType();
            if (canonical->isPointerType() && !canonical->isFunctionPointerType()) {
                return StringRef("_ptr");
            }
            const CXXRecordDecl* record_ptr = canonical->getAsCXXRecordDecl();
            if (isStdClass(record_ptr, {"optional"})) {
                return StringRef("_opt");
            }
            if (isStdClass(record_ptr, {"unique_ptr", "shared_ptr", "weak_ptr"})) {
                return StringRef("_ptr");
            }
            return std::nullopt;
        }

        bool hasPostfix(StringRef name, StringRef postfix) {
            return name.ends_with(postfix) || name == postfix.drop_front();
        }

    } // namespace

    void VariablePostfixCheck::registerMatchers(MatchFinder* finder_ptr) {
        finder_ptr->addMatcher(varDecl(hasLocalStorage(), unless(isImplicit()), unless(isInstantiated())).bind("variable"), this);
    }

    void VariablePostfixCheck::check(const MatchFinder::MatchResult& result) {
        const auto* variable_ptr = result.Nodes.getNodeAs<VarDecl>("variable");
        if (!variable_ptr->getIdentifier() || variable_ptr->getLocation().isMacroID()) {
            return;
        }
        if (const auto* parameter_ptr = dyn_cast<ParmVarDecl>(variable_ptr);
            parameter_ptr && !isOwnParameterName(*parameter_ptr)) {
            return;
        }
        const std::optional<StringRef> postfix_opt = requiredPostfix(variable_ptr->getType());
        if (!postfix_opt.has_value() || hasPostfix(variable_ptr->getName(), postfix_opt.value())) {
            return;
        }
        diag(variable_ptr->getLocation(), "variable '%0' must end with '%1'")
            << variable_ptr->getName() << postfix_opt.value();
    }

} // namespace clang::tidy::eats
