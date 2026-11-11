#include "file_path_check.h"

#include <clang/AST/Decl.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

#include <algorithm>

namespace clang::tidy::eats {

    FilePathCheck::FilePathCheck(StringRef name, ClangTidyContext* context_ptr)
        : ClangTidyCheck(name, context_ptr)
        , sourceRootRegex_(Options.get("SourceRootRegex", ""))
        , excludedPathRegex_(Options.get("ExcludedPathRegex", ""))
        , rootRegex_(sourceRootRegex_)
        , excludeRegex_(excludedPathRegex_)
        , valid_(!sourceRootRegex_.empty())
    {
        std::string error;
        if (valid_ && (!rootRegex_.isValid(error) || rootRegex_.getNumMatches() != 1)) {
            configurationDiag("SourceRootRegex must be a valid regex with one capture group for the root namespace");
            valid_ = false;
        }
        if (!excludedPathRegex_.empty() && !excludeRegex_.isValid(error)) {
            configurationDiag("ExcludedPathRegex must be a valid regex");
            valid_ = false;
        }
    }

    void FilePathCheck::storeOptions(ClangTidyOptions::OptionMap& options) {
        Options.store(options, "SourceRootRegex", sourceRootRegex_);
        Options.store(options, "ExcludedPathRegex", excludedPathRegex_);
    }

    std::optional<FilePathInfo> FilePathCheck::filePath(StringRef fileName) const {
        if (!valid_ || fileName.empty()) {
            return std::nullopt;
        }
        llvm::SmallString<256> path(fileName);
        if (llvm::sys::fs::make_absolute(path)) {
            return std::nullopt;
        }
        llvm::sys::path::remove_dots(path, true);
        const std::string normalized = llvm::sys::path::convert_to_slash(path);
        llvm::SmallVector<StringRef, 2> matches;
        if (!rootRegex_.match(normalized, &matches) || matches[0].data() != normalized.data() ||
            !matches[0].ends_with("/") || matches[1].empty()) {
            return std::nullopt;
        }
        const StringRef relative = StringRef(normalized).drop_front(matches[0].size());
        if (relative.empty() || (!excludedPathRegex_.empty() && excludeRegex_.match(relative))) {
            return std::nullopt;
        }
        std::string expectedNamespace = matches[1].str();
        std::replace(expectedNamespace.begin(), expectedNamespace.end(), '-', '_');
        const StringRef directories = llvm::sys::path::parent_path(relative);
        for (auto it = llvm::sys::path::begin(directories); it != llvm::sys::path::end(directories); ++it) {
            expectedNamespace += "::" + it->str();
        }
        return FilePathInfo{matches[0].str(), relative.str(), std::move(expectedNamespace)};
    }

    std::optional<FilePathInfo> FilePathCheck::filePath(const SourceManager& sourceManager, SourceLocation loc) const {
        if (loc.isInvalid() || loc.isMacroID() || sourceManager.isInSystemHeader(loc)) {
            return std::nullopt;
        }
        return filePath(sourceManager.getFilename(loc));
    }

    std::string namedNamespace(const DeclContext* context_ptr) {
        std::string result;
        while (context_ptr && !context_ptr->isTranslationUnit()) {
            if (const auto* namespace_ptr = dyn_cast<NamespaceDecl>(context_ptr)) {
                if (!namespace_ptr->isAnonymousNamespace()) {
                    result = namespace_ptr->getNameAsString() + (result.empty() ? "" : "::" + result);
                }
            }
            context_ptr = context_ptr->getParent();
        }
        return result;
    }

} // namespace clang::tidy::eats
