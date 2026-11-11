#include "class_file_name_check.h"

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/Path.h>

#include <cctype>

namespace clang::tidy::eats {
    namespace {

        std::string snakeCase(StringRef name) {
            std::string result;
            for (std::size_t index = 0; index < name.size(); ++index) {
                const auto current = static_cast<unsigned char>(name[index]);
                if (std::isupper(current) && index != 0 && name[index - 1] != '_' &&
                    (!std::isupper(static_cast<unsigned char>(name[index - 1])) ||
                     (index + 1 < name.size() && std::islower(static_cast<unsigned char>(name[index + 1]))))) {
                    result += '_';
                }
                result += static_cast<char>(std::tolower(current));
            }
            return result;
        }

        unsigned methodCount(const CXXRecordDecl& declaration) {
            unsigned count = 0;
            for (const auto* member_ptr : declaration.decls()) {
                const FunctionDecl* method_ptr = dyn_cast<CXXMethodDecl>(member_ptr);
                if (const auto* template_ptr = dyn_cast<FunctionTemplateDecl>(member_ptr)) {
                    method_ptr = template_ptr->getTemplatedDecl();
                }
                if (method_ptr && !method_ptr->isImplicit() && !method_ptr->isTemplateInstantiation() &&
                    !isa<CXXConstructorDecl, CXXDestructorDecl>(method_ptr)) {
                    ++count;
                }
            }
            return count;
        }

        bool inAnonymousNamespace(const DeclContext* context_ptr) {
            while (context_ptr && !context_ptr->isTranslationUnit()) {
                if (const auto* namespace_ptr = dyn_cast<NamespaceDecl>(context_ptr)) {
                    if (namespace_ptr->isAnonymousNamespace()) {
                        return true;
                    }
                }
                context_ptr = context_ptr->getParent();
            }
            return false;
        }

    } // namespace

    void ClassFileNameCheck::storeOptions(ClangTidyOptions::OptionMap& options) {
        FilePathCheck::storeOptions(options);
        Options.store(options, "HelperMaxMethods", helperMaxMethods_);
        Options.store(options, "HelperMaxFields", helperMaxFields_);
    }

    void ClassFileNameCheck::registerMatchers(ast_matchers::MatchFinder* finder_ptr) {
        using namespace ast_matchers;
        finder_ptr->addMatcher(cxxRecordDecl(isDefinition(), unless(isImplicit()), unless(isTemplateInstantiation())).bind("class"), this);
    }

    void ClassFileNameCheck::check(const ast_matchers::MatchFinder::MatchResult& result) {
        const auto* declaration_ptr = result.Nodes.getNodeAs<CXXRecordDecl>("class");
        const auto* context_ptr = declaration_ptr->getDeclContext();
        const bool plainData = declaration_ptr->isStruct() && declaration_ptr->isAggregate() &&
                               methodCount(*declaration_ptr) == 0 && !declaration_ptr->hasUserDeclaredDestructor();
        if (plainData || declaration_ptr->getName().empty() ||
            (!context_ptr->isNamespace() && !context_ptr->isTranslationUnit()) || inAnonymousNamespace(context_ptr) ||
            isa<ClassTemplateSpecializationDecl>(declaration_ptr)) {
            return;
        }
        const auto path_opt = filePath(*result.SourceManager, declaration_ptr->getLocation());
        if (!path_opt.has_value() || namedNamespace(context_ptr) != path_opt.value().expectedNamespace) {
            return;
        }
        sourceManager_ptr_ = result.SourceManager;
        llvm::SmallString<256> pairPath(path_opt.value().root);
        pairPath += path_opt.value().relative;
        llvm::sys::path::replace_extension(pairPath, "");
        classes_[pairPath.str().str()].push_back(declaration_ptr);
    }

    void ClassFileNameCheck::onEndOfTranslationUnit() {
        if (!sourceManager_ptr_) {
            return;
        }
        for (const auto& entry : classes_) {
            const StringRef fileName = sourceManager_ptr_->getFilename(entry.second.front()->getLocation());
            const StringRef stem = llvm::sys::path::stem(fileName);
            const bool hasMainClass = llvm::any_of(entry.second, [stem](const CXXRecordDecl* declaration_ptr) {
                return snakeCase(declaration_ptr->getName()) == stem;
            });
            for (const auto* declaration_ptr : entry.second) {
                const std::string expected = snakeCase(declaration_ptr->getName());
                if (expected == stem || (hasMainClass && methodCount(*declaration_ptr) <= helperMaxMethods_ &&
                                         std::distance(declaration_ptr->field_begin(), declaration_ptr->field_end()) <= helperMaxFields_)) {
                    continue;
                }
                diag(declaration_ptr->getLocation(), "class '%0' should be in a file named '%1%2'")
                    << declaration_ptr->getName() << expected
                    << llvm::sys::path::extension(sourceManager_ptr_->getFilename(declaration_ptr->getLocation()));
            }
        }
        classes_.clear();
        sourceManager_ptr_ = nullptr;
    }

} // namespace clang::tidy::eats
