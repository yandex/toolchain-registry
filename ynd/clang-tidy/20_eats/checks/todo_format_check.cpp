#include "todo_format_check.h"

#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <llvm/Support/Regex.h>

namespace clang::tidy::eats {

class TodoFormatCheck::TodoCommentHandler : public CommentHandler {
public:
    explicit TodoCommentHandler(TodoFormatCheck& check)
        : check_(check)
        , todoMarker_("^TODO([^A-Za-z0-9_]|$)")
        , todoFormat_("^TODO\\( *[A-Za-z0-9._-]+ *, *[A-Z][A-Z0-9_]*-[0-9]+ *\\):")
    {}

    bool HandleComment(Preprocessor& preprocessor, SourceRange range) override {
        const SourceManager& sourceManager = preprocessor.getSourceManager();
        if (sourceManager.isInSystemHeader(range.getBegin())) {
            return false;
        }
        const StringRef comment = Lexer::getSourceText(
            CharSourceRange::getCharRange(range), sourceManager, preprocessor.getLangOpts());

        // A block comment may hold a TODO on any of its lines.
        size_t lineOffset = 0;
        while (lineOffset < comment.size()) {
            const size_t lineEnd = std::min(comment.find('\n', lineOffset), comment.size());
            checkLine(range.getBegin(), comment, lineOffset, lineEnd);
            lineOffset = lineEnd + 1;
        }
        return false;
    }

private:
    void checkLine(SourceLocation commentStart, StringRef comment, size_t lineOffset, size_t lineEnd) {
        const StringRef line = comment.slice(lineOffset, lineEnd);
        const StringRef text = line.ltrim(" \t/*!");
        if (!todoMarker_.match(text) || todoFormat_.match(text)) {
            return;
        }
        const size_t textOffset = lineOffset + (line.size() - text.size());
        check_.diag(commentStart.getLocWithOffset(textOffset),
                    "TODO must name its author and a ticket: 'TODO(author, QUEUE-123): ...'");
    }

    TodoFormatCheck& check_;
    llvm::Regex todoMarker_;
    llvm::Regex todoFormat_;
};

TodoFormatCheck::TodoFormatCheck(StringRef name, ClangTidyContext* context)
    : ClangTidyCheck(name, context)
    , handler_(std::make_unique<TodoCommentHandler>(*this))
{}

TodoFormatCheck::~TodoFormatCheck() = default;

void TodoFormatCheck::registerPPCallbacks(const SourceManager&, Preprocessor* preprocessor, Preprocessor*) {
    preprocessor->addCommentHandler(handler_.get());
}

}  // namespace clang::tidy::eats
