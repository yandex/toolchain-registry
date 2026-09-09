#include "SdcIncludeOrderCheck.h"
#include "SdcPreprocessorFileCollector.h"
namespace clang::tidy::sdc {
namespace {
class Callbacks final : public PreprocessorFileCollector {
public:
    Callbacks(SdcIncludeOrderCheck &Check, Preprocessor &PP)
        : PreprocessorFileCollector(PP), Check(Check) {}
    void EndOfMainFile() override {
        for (FileID FID : Files) {
            auto Start = SM.getLocForStartOfFile(FID);
            bool Invalid = false;
            StringRef Text = SM.getBufferData(FID, &Invalid);
            if (Invalid) continue;
            Lexer Lex(Start, PP.getLangOpts(), Text.begin(), Text.begin(), Text.end());
            std::vector<Token> Tokens;
            Token T;
            while (!Lex.LexFromRawLexer(T)) Tokens.push_back(T);
            if (!T.is(tok::eof)) Tokens.push_back(T);
            auto Spelling = [&](size_t I) { return Lexer::getSpelling(Tokens[I], SM, PP.getLangOpts()); };
            bool SeenCode = false;
            unsigned LinkageBraces = 0;
            for (size_t I = 0; I < Tokens.size();) {
                const auto &Tok = Tokens[I];
                if (Tok.is(tok::hash) && Tok.isAtStartOfLine()) {
                    size_t End = I + 1;
                    while (End < Tokens.size() && !Tokens[End].isAtStartOfLine()) ++End;
                    if (I + 1 < End) {
                        auto Name = Spelling(I+1);
                        if (Name == "include" && SeenCode)
                            Check.diag(Tok.getLocation(), "place includes before source code, except linkage-specification delimiters");
                    }
                    I = End; continue;
                }
                if (Spelling(I) == "extern" && I+1 < Tokens.size() &&
                    (Spelling(I+1) == "\"C\"" || Spelling(I+1) == "\"C++\"")) {
                    I += 2;
                    if (I < Tokens.size() && Tokens[I].is(tok::l_brace)) { ++LinkageBraces; ++I; }
                    continue;
                }
                if (Tok.is(tok::r_brace) && LinkageBraces && !SeenCode) { --LinkageBraces; ++I; continue; }
                SeenCode = true; ++I;
            }
        }
    }
private:
    SdcIncludeOrderCheck &Check;
};
} // namespace
void SdcIncludeOrderCheck::registerPPCallbacks(const SourceManager &, Preprocessor *PP, Preprocessor *) {
    PP->addPPCallbacks(std::make_unique<Callbacks>(*this, *PP));
}
} // namespace clang::tidy::sdc
