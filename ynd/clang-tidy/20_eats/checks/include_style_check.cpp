#include "include_style_check.h"

#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/Support/Path.h>

namespace clang::tidy::eats {
    namespace {

        class IncludeStyleCallbacks: public PPCallbacks {
        public:
            IncludeStyleCallbacks(IncludeStyleCheck& check, const SourceManager& sourceManager)
                : check_(check)
                , sourceManager_(sourceManager)
            {
            }

            void InclusionDirective(SourceLocation hashLoc, const Token&, StringRef fileName, bool isAngled,
                                    CharSourceRange filenameRange, OptionalFileEntryRef file_opt, StringRef, StringRef,
                                    const Module*, bool, SrcMgr::CharacteristicKind fileType) override {
                const bool first = seenFiles_.insert(sourceManager_.getFileID(hashLoc).getHashValue()).second;
                const auto source_opt = check_.filePath(sourceManager_, hashLoc);
                if (!source_opt.has_value() || !file_opt.has_value() || fileType != SrcMgr::C_User ||
                    filenameRange.getBegin().isMacroID()) {
                    return;
                }
                const auto target_opt = check_.filePath((*file_opt).getName());
                if (!target_opt.has_value() || source_opt.value().root != target_opt.value().root) {
                    return;
                }
                const StringRef target(target_opt.value().relative);
                if (llvm::sys::path::extension(target) != ".h" && llvm::sys::path::extension(target) != ".hpp") {
                    return;
                }
                const StringRef source(source_opt.value().relative);
                const bool ownHeader = llvm::sys::path::extension(source) == ".cpp" &&
                                       llvm::sys::path::parent_path(source) == llvm::sys::path::parent_path(target) &&
                                       llvm::sys::path::stem(source) == llvm::sys::path::stem(target);
                if (ownHeader) {
                    const StringRef basename = llvm::sys::path::filename(target);
                    if (isAngled || fileName != basename) {
                        check_.diag(filenameRange.getBegin(), "include the source file's own header with quotes and no directory")
                            << FixItHint::CreateReplacement(filenameRange, "\"" + basename.str() + "\"");
                    }
                    if (!first) {
                        check_.diag(hashLoc, "include the source file's own header before other headers");
                    }
                } else if (!isAngled || fileName != target) {
                    check_.diag(filenameRange.getBegin(), "include project headers with angle brackets and a source-root-relative path")
                        << FixItHint::CreateReplacement(filenameRange, "<" + target.str() + ">");
                }
            }

        private:
            IncludeStyleCheck& check_;
            const SourceManager& sourceManager_;
            llvm::DenseSet<unsigned> seenFiles_;
        };

    } // namespace

    void IncludeStyleCheck::registerPPCallbacks(const SourceManager& sourceManager, Preprocessor* preprocessor_ptr,
                                                Preprocessor*) {
        preprocessor_ptr->addPPCallbacks(std::make_unique<IncludeStyleCallbacks>(*this, sourceManager));
    }

} // namespace clang::tidy::eats
