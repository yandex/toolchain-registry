#pragma once
#include "SdcCodeSelection.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include <set>

namespace clang::tidy::sdc {
class PreprocessorFileCollector : public PPCallbacks {
public:
    explicit PreprocessorFileCollector(Preprocessor &PP) : PP(PP), SM(PP.getSourceManager()) {}
    void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                     SrcMgr::CharacteristicKind, FileID) override {
        if (Reason != EnterFile) return;
        FileID FID = SM.getFileID(Loc);
        if (FID.isValid() && SM.getFileEntryRefForID(FID) && isWrittenInAnalyzedSource(Loc, SM))
            Files.insert(FID);
    }
protected:
    Preprocessor &PP;
    SourceManager &SM;
    std::set<FileID> Files;
};
} // namespace clang::tidy::sdc
