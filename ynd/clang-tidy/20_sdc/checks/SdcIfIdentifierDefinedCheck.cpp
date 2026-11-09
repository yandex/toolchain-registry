#include "SdcIfIdentifierDefinedCheck.h"

#include "clang/Lex/Lexer.h"
#include "clang/Lex/MacroArgs.h"
#include "llvm/ADT/DenseSet.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallPtrSet.h"

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

    llvm::DenseSet<unsigned> UnevaluatedArguments;

    std::string spelling(const Token& T) const {
        return Lexer::getSpelling(T, SM, LangOpts);
    }

    // Determine whether a parameter's spelling survives into an evaluated
    // expression. Do not re-run macro expansion: that would mutate builtin
    // state (e.g. __COUNTER__) while processing a preprocessor callback.
    bool needsDefinition(ArrayRef<Token> Tokens, StringRef Parameter,
                         llvm::SmallPtrSetImpl<const MacroInfo*>& Active) {
        for (size_t I = 0; I < Tokens.size(); ++I) {
            const std::string Name = spelling(Tokens[I]);
            if (Name == "defined") {
                if (I + 1 < Tokens.size() && Tokens[I + 1].is(tok::l_paren)) I += 3;
                else ++I;
                continue;
            }
            if (Name == Parameter) {
                // Stringification and concatenation consume tokens rather than
                // evaluating the original identifier as a controlling operand.
                if ((I && (Tokens[I-1].is(tok::hash) || Tokens[I-1].is(tok::hashhash))) ||
                    (I+1 < Tokens.size() && Tokens[I+1].is(tok::hashhash))) continue;
                return true;
            }
            if (!Tokens[I].isOneOf(tok::identifier, tok::raw_identifier) ||
                I+1 >= Tokens.size() || !Tokens[I+1].is(tok::l_paren)) continue;
            const MacroInfo* MI = PP.getMacroInfo(PP.getIdentifierInfo(Name));
            if (!MI) continue;
            const bool Query = MI->isBuiltinMacro() &&
                (StringRef(Name).starts_with("__has_") || Name == "__is_identifier");
            if (!Query && (!MI->isFunctionLike() || Active.contains(MI))) continue;
            size_t Begin = I+2;
            unsigned Depth = 0, ArgIndex = 0;
            bool Closed = false;
            for (size_t J = Begin; J < Tokens.size(); ++J) {
                if (Tokens[J].is(tok::l_paren)) ++Depth;
                else if (Tokens[J].is(tok::r_paren) && Depth) --Depth;
                else if ((Tokens[J].is(tok::comma) && !Depth) ||
                         (Tokens[J].is(tok::r_paren) && !Depth)) {
                    bool Used = false;
                    if (!Query) {
                        // Variadic/unusual forms remain conservative.
                        Used = ArgIndex >= MI->getNumParams();
                        if (!Used) {
                            Active.insert(MI);
                            Used = needsDefinition(MI->tokens(), MI->params()[ArgIndex]->getName(), Active);
                            Active.erase(MI);
                        }
                    }
                    if (Used && needsDefinition(Tokens.slice(Begin, J-Begin), Parameter, Active)) return true;
                    ++ArgIndex;
                    Begin = J+1;
                    if (Tokens[J].is(tok::r_paren)) { I = J; Closed = true; break; }
                }
            }
            if (!Closed) return true;
        }
        return false;
    }

    void inspect(SourceRange Range) {
        if (Range.isInvalid() || SM.isInSystemHeader(Range.getBegin())) return;

        // Conditions can start/end with expanded macros. Scan their invocation
        // in the directive, never the replacement text in another source buffer.
        CharSourceRange FileRange = Lexer::makeFileCharRange(
            SM.getExpansionRange(Range), SM, LangOpts);
        if (FileRange.isInvalid()) return;
        SourceLocation Cur = FileRange.getBegin();
        SourceLocation End = FileRange.getEnd();
        const FileID File = SM.getFileID(Cur);
        if (SM.getFileID(End) != File) return;
        const unsigned EndOffset = SM.getFileOffset(End);
        llvm::SmallSet<std::string, 4> GuardedNames;
        bool AwaitDefinedOperand = false;
        std::string DefinedCandidate;

        while (Cur.isValid() && SM.getFileOffset(Cur) < EndOffset) {
            Token Tok;
            if (Lexer::getRawToken(Cur, Tok, SM, LangOpts,
                                   /*IgnoreWhiteSpace=*/true))
                break;

            // Ignoring whitespace may advance to a token past the condition.
            if (SM.getFileID(Tok.getLocation()) != File ||
                SM.getFileOffset(Tok.getLocation()) >= EndOffset)
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

            if (UnevaluatedArguments.contains(SM.getSpellingLoc(Tok.getLocation()).getRawEncoding()))
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

    void MacroExpands(const Token& MacroName, const MacroDefinition& Definition,
                      SourceRange, const MacroArgs* Args) override {
        if (!PP.isParsingIfOrElifDirective() || MacroName.getLocation().isMacroID()) return;
        const MacroInfo* MI = Definition.getMacroInfo();
        if (!MI || !Args || !MI->isFunctionLike() || MI->isVariadic()) return;
        for (unsigned I = 0; I < MI->getNumParams(); ++I) {
            llvm::SmallPtrSet<const MacroInfo*, 8> Active;
            Active.insert(MI);
            if (needsDefinition(MI->tokens(), MI->params()[I]->getName(), Active)) continue;
            for (const Token* T = Args->getUnexpArgument(I); !T->is(tok::eof); ++T)
                UnevaluatedArguments.insert(SM.getSpellingLoc(T->getLocation()).getRawEncoding());
        }
    }

    void If(SourceLocation, SourceRange Range, ConditionValueKind) override {
        inspect(Range);
        UnevaluatedArguments.clear();
    }

    void Elif(SourceLocation, SourceRange Range, ConditionValueKind,
              SourceLocation) override {
        inspect(Range);
        UnevaluatedArguments.clear();
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
