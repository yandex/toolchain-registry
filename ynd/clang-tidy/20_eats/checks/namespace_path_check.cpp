#include "namespace_path_check.h"

namespace clang::tidy::eats {
    namespace {

        bool namespacePrefix(StringRef prefix, StringRef name) {
            return prefix.empty() || name == prefix || (name.starts_with(prefix) && name.drop_front(prefix.size()).starts_with("::"));
        }

        bool containsOnlyNamespaces(const NamespaceDecl& declaration) {
            for (const auto* child_ptr : declaration.decls()) {
                if (!isa<NamespaceDecl, EmptyDecl>(child_ptr)) {
                    return false;
                }
            }
            return true;
        }

        bool isTypeForwardDeclaration(const Decl& declaration) {
            if (const auto* tag_ptr = dyn_cast<TagDecl>(&declaration)) {
                return !tag_ptr->isThisDeclarationADefinition();
            }
            if (const auto* template_ptr = dyn_cast<ClassTemplateDecl>(&declaration)) {
                return !template_ptr->getTemplatedDecl()->isThisDeclarationADefinition();
            }
            return false;
        }

        bool containsOnlyTypeForwardDeclarations(const NamespaceDecl& declaration) {
            for (const auto* child_ptr : declaration.decls()) {
                if (const auto* namespace_ptr = dyn_cast<NamespaceDecl>(child_ptr)) {
                    if (!containsOnlyTypeForwardDeclarations(*namespace_ptr)) {
                        return false;
                    }
                } else if (!isa<EmptyDecl>(child_ptr) && !isTypeForwardDeclaration(*child_ptr)) {
                    return false;
                }
            }
            return true;
        }

    } // namespace

    void NamespacePathCheck::registerMatchers(ast_matchers::MatchFinder* finder_ptr) {
        using namespace ast_matchers;
        finder_ptr->addMatcher(namespaceDecl(unless(isAnonymous())).bind("namespace"), this);
        finder_ptr->addMatcher(namedDecl(anyOf(functionDecl(unless(isMain())), varDecl(), cxxRecordDecl(), enumDecl(),
                                               typedefNameDecl(), conceptDecl()),
                                         hasDeclContext(translationUnitDecl()), unless(isImplicit()))
                                   .bind("global"), this);
    }

    void NamespacePathCheck::check(const ast_matchers::MatchFinder::MatchResult& result) {
        const auto* declaration_ptr = result.Nodes.getNodeAs<NamespaceDecl>("namespace");
        const auto* global_ptr = result.Nodes.getNodeAs<NamedDecl>("global");
        const SourceLocation loc = declaration_ptr ? declaration_ptr->getLocation() : global_ptr->getLocation();
        const auto path_opt = filePath(*result.SourceManager, loc);
        if (!path_opt.has_value() || (global_ptr && isTypeForwardDeclaration(*global_ptr))) {
            return;
        }
        const StringRef expected(path_opt.value().expectedNamespace);
        if (declaration_ptr) {
            const std::string actual = namedNamespace(declaration_ptr);
            const std::string parent = namedNamespace(declaration_ptr->getParent());
            if (containsOnlyTypeForwardDeclarations(*declaration_ptr) || namespacePrefix(expected, actual) ||
                (namespacePrefix(actual, expected) && containsOnlyNamespaces(*declaration_ptr)) ||
                (!namespacePrefix(parent, expected) && !namespacePrefix(expected, parent))) {
                return;
            }
        }
        diag(loc, "declarations in this file should be in namespace '%0'") << expected;
    }

} // namespace clang::tidy::eats
