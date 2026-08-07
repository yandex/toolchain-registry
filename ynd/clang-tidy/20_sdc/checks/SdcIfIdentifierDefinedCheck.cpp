#include "SdcIfIdentifierDefinedCheck.h"

#include "clang/Lex/Lexer.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/SmallSet.h"

namespace clang {
namespace tidy {
namespace sdc {

SdcIfIdentifierDefinedCheck::SdcIfIdentifierDefinedCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

namespace {

class IfIdentifierCallbacks : public PPCallbacks {
    SdcIfIdentifierDefinedCheck& Check;
    const SourceManager& SM;
    Preprocessor& PP;
    const LangOptions& LangOpts;

    void inspect(SourceRange Range) {
        if (Range.isInvalid() || SM.isInSystemHeader(Range.getBegin())) return;

        SourceLocation Cur = SM.getSpellingLoc(Range.getBegin());
        SourceLocation Last = SM.getSpellingLoc(Range.getEnd());
        llvm::SmallSet<std::string, 4> GuardedNames;
        bool AwaitDefinedOperand = false;
        std::string DefinedCandidate;

        while (Cur.isValid() &&
               !SM.isBeforeInTranslationUnit(Last, Cur)) {
            Token Tok;
            if (Lexer::getRawToken(Cur, Tok, SM, LangOpts,
                                   /*IgnoreWhiteSpace=*/true))
                break;

            SourceLocation Next = Lexer::getLocForEndOfToken(
                Tok.getLocation(), 0, SM, LangOpts);
            if (Next.isInvalid() || Next == Cur) break;
            Cur = Next;

            if (Tok.is(tok::ampamp) && !DefinedCandidate.empty()) {
                GuardedNames.insert(DefinedCandidate);
                DefinedCandidate.clear();
                continue;
            }
            if (Tok.is(tok::pipepipe)) {
                DefinedCandidate.clear();
                continue;
            }
            if (!Tok.is(tok::raw_identifier) && !Tok.is(tok::identifier))
                continue;

            std::string Name = Lexer::getSpelling(Tok, SM, LangOpts);
            if (Name == "defined") {
                AwaitDefinedOperand = true;
                continue;
            }
            if (Name == "if" || Name == "elif") continue;
            if (AwaitDefinedOperand) {
                DefinedCandidate = Name;
                AwaitDefinedOperand = false;
                continue;
            }

            // Preserve the standard defined(X) && X guard idiom already used
            // throughout the local suite.  Every other identifier in the
            // controlling expression must name a macro at this point.
            if (GuardedNames.count(Name) || PP.isMacroDefined(Name))
                continue;
            if (StringRef(Name).starts_with("__has_")) continue;

            Check.diag(Tok.getLocation(),
                       "identifier '%0' in a preprocessing controlling "
                       "expression shall be defined before evaluation")
                << Name;
        }
    }

public:
    IfIdentifierCallbacks(SdcIfIdentifierDefinedCheck& C,
                          const SourceManager& SM, Preprocessor& PP)
        : Check(C), SM(SM), PP(PP), LangOpts(PP.getLangOpts()) {}

    void If(SourceLocation, SourceRange Range, ConditionValueKind) override {
        inspect(Range);
    }

    void Elif(SourceLocation, SourceRange Range, ConditionValueKind,
              SourceLocation) override {
        inspect(Range);
    }
};

} // namespace

void SdcIfIdentifierDefinedCheck::registerPPCallbacks(
    const SourceManager& SM, Preprocessor* PP, Preprocessor*) {
    PP->addPPCallbacks(
        std::make_unique<IfIdentifierCallbacks>(*this, SM, *PP));
}

} // namespace sdc
} // namespace tidy
} // namespace clang
