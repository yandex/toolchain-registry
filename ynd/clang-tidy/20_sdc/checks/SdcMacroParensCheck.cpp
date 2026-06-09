#include "SdcMacroParensCheck.h"
#include "SdcPreprocessorUtils.h"

#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

namespace clang {
namespace tidy {
namespace sdc {

SdcMacroParensCheck::SdcMacroParensCheck(StringRef Name,
                                          ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

namespace {

static bool isCriticalOp(tok::TokenKind K) {
    switch (K) {
    // Multiplicative
    case tok::star: case tok::slash: case tok::percent:
    // Additive (binary — unary forms filtered separately)
    case tok::plus: case tok::minus:
    // Shift
    case tok::lessless: case tok::greatergreater:
    // Relational / equality
    case tok::less: case tok::greater:
    case tok::lessequal: case tok::greaterequal:
    case tok::equalequal: case tok::exclaimequal:
    // Bitwise / logical
    case tok::amp: case tok::caret: case tok::pipe:
    case tok::ampamp: case tok::pipepipe:
    // Ternary / assignment
    case tok::question:
    case tok::equal:
    case tok::plusequal: case tok::minusequal:
    case tok::starequal: case tok::slashequal: case tok::percentequal:
    case tok::lesslessequal: case tok::greatergreaterequal:
    case tok::ampequal: case tok::caretequal: case tok::pipeequal:
        return true;
    default:
        return false;
    }
}

// Returns true if K is a token that, in context, is likely unary
// (so + or - following it is also unary, not a critical binary op).
static bool isUnaryContext(tok::TokenKind K) {
    switch (K) {
    case tok::l_paren:
    case tok::equal: case tok::plusequal: case tok::minusequal:
    case tok::starequal: case tok::slashequal: case tok::percentequal:
    case tok::lesslessequal: case tok::greatergreaterequal:
    case tok::ampequal: case tok::caretequal: case tok::pipeequal:
    case tok::plus: case tok::minus: case tok::amp:
    case tok::exclaim: case tok::tilde:
    case tok::star: case tok::slash: case tok::percent:
    case tok::lessless: case tok::greatergreater:
    case tok::less: case tok::greater: case tok::lessequal:
    case tok::greaterequal: case tok::equalequal: case tok::exclaimequal:
    case tok::caret: case tok::pipe: case tok::ampamp: case tok::pipepipe:
    case tok::question: case tok::colon: case tok::comma:
        return true;
    default:
        return false;
    }
}

// Returns true if the pre-expanded argument token sequence contains a
// top-level BINARY critical operator (level <= 0, not the first token,
// not preceded by a context that implies unary interpretation).
static bool hasTopLevelCriticalOp(const std::vector<Token>& Toks) {
    int Level = 0;
    tok::TokenKind PrevKind = tok::unknown; // unary context at start
    bool PrevWasOperator = true; // treat start as operator context

    for (const auto& T : Toks) {
        if (T.is(tok::eof)) break;
        tok::TokenKind K = T.getKind();

        if (K == tok::l_paren) {
            Level++;
            PrevWasOperator = true;
            PrevKind = K;
            continue;
        }
        if (K == tok::r_paren) {
            if (Level > 0) Level--;
            PrevWasOperator = false;
            PrevKind = K;
            continue;
        }

        if (Level <= 0 && isCriticalOp(K)) {
            // For potentially-unary operators, skip if in unary context.
            bool potentiallyUnary = (K == tok::plus || K == tok::minus ||
                                     K == tok::amp  || K == tok::star  ||
                                     K == tok::exclaim || K == tok::tilde);
            if (!potentiallyUnary || !PrevWasOperator)
                return true;
        }

        PrevWasOperator = isUnaryContext(K);
        PrevKind = K;
    }
    return false;
}

// Returns true when all occurrences of parameter ParamIdx in MI's body are
// either directly parenthesized (( param )) or stringified (# param).
static bool isParamProtected(const MacroInfo* MI, unsigned ParamIdx) {
    const auto Toks = MI->tokens();
    const unsigned N = Toks.size();

    for (unsigned I = 0; I < N; ++I) {
        int Idx = ::sdc::pp::paramIndex(Toks[I], MI);
        if (Idx < 0 || static_cast<unsigned>(Idx) != ParamIdx) continue;

        // Stringified occurrence is always protected.
        if (I > 0 && ::sdc::pp::isStringify(Toks[I - 1])) continue;

        // Directly parenthesized: immediately surrounded by ( ).
        bool lparen = (I > 0 && Toks[I - 1].is(tok::l_paren));
        bool rparen = (I + 1 < N && Toks[I + 1].is(tok::r_paren));
        if (lparen && rparen) continue;

        return false; // Unprotected occurrence found
    }
    return true; // All occurrences protected
}

struct MacroParamInfo {
    llvm::SmallVector<bool, 8> Protected; // Protected[i] = true iff param i is protected
};

class MacroParensCallbacks : public PPCallbacks {
    SdcMacroParensCheck& Check;
    const SourceManager& SM;
    Preprocessor& PP;

    llvm::DenseMap<const IdentifierInfo*, MacroParamInfo> MacroInfo_;

public:
    MacroParensCallbacks(SdcMacroParensCheck& C, const SourceManager& SM,
                          Preprocessor& PP)
        : Check(C), SM(SM), PP(PP) {}

    void MacroDefined(const Token& MacroNameTok,
                      const MacroDirective* MD) override {
        if (::sdc::pp::isSystemHeader(MacroNameTok.getLocation(), SM)) return;

        const MacroInfo* MI = MD->getMacroInfo();
        if (!MI || !MI->isFunctionLike() || MI->getNumParams() == 0) return;

        MacroParamInfo Info;
        Info.Protected.resize(MI->getNumParams());
        for (unsigned I = 0; I < MI->getNumParams(); ++I)
            Info.Protected[I] = isParamProtected(MI, I);

        MacroInfo_[MacroNameTok.getIdentifierInfo()] = std::move(Info);
    }

    void MacroExpands(const Token& MacroNameTok, const MacroDefinition& MD,
                      SourceRange /*Range*/,
                      const MacroArgs* Args) override {
        if (::sdc::pp::isSystemHeader(MacroNameTok.getLocation(), SM)) return;
        if (!Args) return;

        const IdentifierInfo* Name = MacroNameTok.getIdentifierInfo();
        auto It = MacroInfo_.find(Name);
        if (It == MacroInfo_.end()) return;

        const MacroInfo* MI = MD.getMacroInfo();
        if (!MI) return;

        const auto& Info = It->second;

        for (unsigned I = 0;
             I < MI->getNumParams() && I < Info.Protected.size(); ++I) {
            if (Info.Protected[I]) continue;

            // Pre-expand the argument to see its effective tokens after all
            // contained macros have been expanded.
            const std::vector<Token>& PreExp =
                const_cast<MacroArgs*>(Args)->getPreExpArgument(I, PP);

            if (hasTopLevelCriticalOp(PreExp)) {
                auto PIt = MI->param_begin();
                std::advance(PIt, I);
                StringRef ParamName = (PIt != MI->param_end())
                                          ? (*PIt)->getName()
                                          : "";
                Check.diag(MacroNameTok.getLocation(),
                            "argument for unparenthesized macro parameter '%0' "
                            "in '%1' contains a top-level critical operator; "
                            "wrap the argument or the parameter in parentheses")
                    << ParamName
                    << MacroNameTok.getIdentifierInfo()->getName();
            }
        }
    }
};

} // namespace

void SdcMacroParensCheck::registerPPCallbacks(const SourceManager& SM,
                                               Preprocessor* PP,
                                               Preprocessor* /*MEP*/) {
    PP->addPPCallbacks(
        std::make_unique<MacroParensCallbacks>(*this, SM, *PP));
}

} // namespace sdc
} // namespace tidy
} // namespace clang
