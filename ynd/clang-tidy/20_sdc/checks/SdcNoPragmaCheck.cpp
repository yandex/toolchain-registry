#include "SdcNoPragmaCheck.h"
#include "SdcPreprocessorFileCollector.h"
namespace clang::tidy::sdc {
namespace {
class Callbacks final : public PreprocessorFileCollector {
public:
    Callbacks(SdcNoPragmaCheck &Check, Preprocessor &PP)
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
            for (size_t I = 0; I < Tokens.size();) {
                const auto &Tok = Tokens[I];
                if (Tok.is(tok::hash) && Tok.isAtStartOfLine()) {
                    size_t End = I + 1;
                    while (End < Tokens.size() && !Tokens[End].isAtStartOfLine()) ++End;
                    if (I + 1 < End) {
                        auto Name = Spelling(I+1);
                        if (Name == "pragma" &&
                            !(I+3 == End && Spelling(I+2) == "once"))
                            Check.diag(Tok.getLocation(), "do not use pragma directives other than the project-approved #pragma once");
                        if (Name == "define")
                            for (size_t J = I+2; J < End; ++J)
                                if (Spelling(J) == "_Pragma")
                                    Check.diag(Tokens[J].getLocation(), "do not use the _Pragma operator");
                    }
                    I = End; continue;
                }
                if (Spelling(I) == "_Pragma")
                    Check.diag(Tok.getLocation(), "do not use the _Pragma operator");
                ++I;
            }
        }
    }
private:
    SdcNoPragmaCheck &Check;
};
} // namespace
void SdcNoPragmaCheck::registerPPCallbacks(const SourceManager &, Preprocessor *PP, Preprocessor *) {
    PP->addPPCallbacks(std::make_unique<Callbacks>(*this, *PP));
}
} // namespace clang::tidy::sdc
