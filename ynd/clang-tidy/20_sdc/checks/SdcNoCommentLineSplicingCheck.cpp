#include "SdcNoCommentLineSplicingCheck.h"
#include "SdcPreprocessorFileCollector.h"

namespace clang::tidy::sdc {
namespace {
class Callbacks final : public PreprocessorFileCollector {
public:
    Callbacks(SdcNoCommentLineSplicingCheck& Check, Preprocessor& PP)
        : PreprocessorFileCollector(PP), Check(Check) {}
    void EndOfMainFile() override {
        for (FileID FID : Files) {
            const auto Start = SM.getLocForStartOfFile(FID);
            bool Invalid = false;
            StringRef Text = SM.getBufferData(FID, &Invalid);
            if (Invalid) continue;
            Lexer Lex(Start, PP.getLangOpts(), Text.begin(), Text.begin(), Text.end());
            Lex.SetCommentRetentionState(true);
            Token T;
            do {
                Lex.LexFromRawLexer(T);
                if (!T.is(tok::comment)) continue;
                const unsigned Offset = SM.getFileOffset(T.getLocation());
                StringRef Comment = Text.substr(Offset, T.getLength());
                // Token spelling identifies the comment after escaped-newline
                // cleaning; retain physical bytes for diagnostic locations.
                if (!StringRef(Lexer::getSpelling(T, SM, PP.getLangOpts())).starts_with("//")) continue;
                // The splice forming the opener precedes the comment body.
                const size_t BodyStart = Comment.find('/', 1) + 1;
                for (size_t I = BodyStart; I < Comment.size(); ++I) {
                    if (Comment[I] != '\\') continue;
                    size_t End = I + 1;
                    // Clang also accepts whitespace before the newline.
                    while (End < Comment.size() &&
                           (Comment[End] == ' ' || Comment[End] == '\t')) ++End;
                    if (End < Comment.size() &&
                        (Comment[End] == '\n' || Comment[End] == '\r')) {
                        Check.diag(Start.getLocWithOffset(Offset + I),
                                   "line-splicing shall not be used in // comments");
                    }
                }
            } while (!T.is(tok::eof));
        }
    }
private:
    SdcNoCommentLineSplicingCheck& Check;
};
}
void SdcNoCommentLineSplicingCheck::registerPPCallbacks(
    const SourceManager&, Preprocessor* PP, Preprocessor*) {
    PP->addPPCallbacks(std::make_unique<Callbacks>(*this, *PP));
}
}
