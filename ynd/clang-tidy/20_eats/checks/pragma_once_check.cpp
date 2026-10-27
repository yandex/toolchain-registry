#include "pragma_once_check.h"

#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Support/Path.h>

namespace clang::tidy::eats {

namespace {

bool lexesWord(Lexer& lexer, StringRef word) {
    Token token;
    lexer.LexFromRawLexer(token);
    return token.is(tok::raw_identifier) && !token.isAtStartOfLine() && token.getRawIdentifier() == word;
}

class HeaderStartCallbacks : public PPCallbacks {
public:
    HeaderStartCallbacks(PragmaOnceCheck& check, const SourceManager& sourceManager, const LangOptions& langOpts,
                         const FileExtensionsSet& headerFileExtensions)
        : check_(check)
        , sourceManager_(sourceManager)
        , langOpts_(langOpts)
        , headerFileExtensions_(headerFileExtensions)
    {}

    void FileChanged(SourceLocation loc, FileChangeReason reason, SrcMgr::CharacteristicKind fileType,
                     FileID) override {
        if (reason != EnterFile || fileType != SrcMgr::C_User) {
            return;
        }
        const FileID fileId = sourceManager_.getFileID(loc);
        if (fileId == sourceManager_.getMainFileID()) {
            return;
        }
        const auto file = sourceManager_.getFileEntryRefForID(fileId);
        if (!file || !isHeader(file->getName()) || !checkedFiles_.insert(&file->getFileEntry()).second) {
            return;
        }
        checkHeaderStart(fileId);
    }

private:
    bool isHeader(StringRef fileName) const {
        StringRef extension = llvm::sys::path::extension(fileName);
        extension.consume_front(".");
        return headerFileExtensions_.count(extension) != 0;
    }

    // The raw lexer skips comments, so a header may start with a comment before #pragma once.
    void checkHeaderStart(FileID fileId) {
        const SourceLocation fileStart = sourceManager_.getLocForStartOfFile(fileId);
        const StringRef buffer = sourceManager_.getBufferData(fileId);
        Lexer lexer(fileStart, langOpts_, buffer.begin(), buffer.begin(), buffer.end());

        Token firstToken;
        lexer.LexFromRawLexer(firstToken);
        if (firstToken.is(tok::hash) && lexesWord(lexer, "pragma") && lexesWord(lexer, "once")) {
            return;
        }
        const SourceLocation diagLoc = firstToken.is(tok::eof) ? fileStart : firstToken.getLocation();
        check_.diag(diagLoc, "header must start with '#pragma once'");
    }

    PragmaOnceCheck& check_;
    const SourceManager& sourceManager_;
    const LangOptions& langOpts_;
    const FileExtensionsSet& headerFileExtensions_;
    llvm::SmallPtrSet<const FileEntry*, 32> checkedFiles_;
};

}  // namespace

void PragmaOnceCheck::registerPPCallbacks(const SourceManager& sourceManager, Preprocessor* preprocessor,
                                          Preprocessor*) {
    preprocessor->addPPCallbacks(
        std::make_unique<HeaderStartCallbacks>(*this, sourceManager, getLangOpts(), headerFileExtensions_));
}

}  // namespace clang::tidy::eats
