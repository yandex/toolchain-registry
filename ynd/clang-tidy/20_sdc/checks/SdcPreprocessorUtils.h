#pragma once

#include "clang/Basic/SourceManager.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Token.h"

namespace sdc {
namespace pp {

// ─── Token predicates for macro replacement-list analysis ────────────────────

// Returns true if Tok is a # stringification operator in the replacement list.
inline bool isStringify(const clang::Token& Tok) {
    return Tok.is(clang::tok::hash);
}

// Returns true if Tok is a ## token-paste operator in the replacement list.
inline bool isPaste(const clang::Token& Tok) {
    return Tok.is(clang::tok::hashhash);
}

// Returns true if Tok refers to a named parameter of MI.
inline bool isParam(const clang::Token& Tok, const clang::MacroInfo* MI) {
    if (!Tok.is(clang::tok::identifier)) return false;
    const clang::IdentifierInfo* II = Tok.getIdentifierInfo();
    return MI->getParameterNum(II) >= 0;
}

// Returns the 0-based parameter index for Tok, or -1 if not a param.
inline int paramIndex(const clang::Token& Tok, const clang::MacroInfo* MI) {
    if (!Tok.is(clang::tok::identifier)) return -1;
    const clang::IdentifierInfo* II = Tok.getIdentifierInfo();
    return MI->getParameterNum(II);
}

// ─── Parameter-usage classification (for 19.3.3) ─────────────────────────────

// How a single macro parameter is used in the replacement list.
struct ParamUsage {
    bool InStringify = false;  // appears after # (stringification)
    bool InPaste     = false;  // appears immediately before or after ##
    bool InPlain     = false;  // appears without any # / ## context
};

// Returns usage information for every parameter of MI.
// Result[i] describes how parameter i is used.
inline llvm::SmallVector<ParamUsage, 8>
classifyParamUsage(const clang::MacroInfo* MI) {
    llvm::SmallVector<ParamUsage, 8> Result(MI->getNumParams());
    const auto Toks = MI->tokens();
    const unsigned N = Toks.size();

    for (unsigned I = 0; I < N; ++I) {
        int PIdx = paramIndex(Toks[I], MI);
        if (PIdx < 0) continue;

        // Is the token immediately preceded by # (stringification)?
        bool prevIsHash = (I > 0 && isStringify(Toks[I - 1]));
        // Is the token immediately preceded by ##?
        bool prevIsPaste = (I > 0 && isPaste(Toks[I - 1]));
        // Is the token immediately followed by ##?
        bool nextIsPaste = (I + 1 < N && isPaste(Toks[I + 1]));

        if (prevIsHash)
            Result[static_cast<unsigned>(PIdx)].InStringify = true;
        else if (prevIsPaste || nextIsPaste)
            Result[static_cast<unsigned>(PIdx)].InPaste = true;
        else
            Result[static_cast<unsigned>(PIdx)].InPlain = true;
    }
    return Result;
}

// Returns true if PU describes a "mixed-use" parameter (used in more than one
// context), which is what 19.3.3 prohibits.
inline bool isMixedUse(const ParamUsage& PU) {
    int Contexts = (PU.InStringify ? 1 : 0)
                 + (PU.InPaste     ? 1 : 0)
                 + (PU.InPlain     ? 1 : 0);
    return Contexts > 1;
}

// ─── Source-location helpers ─────────────────────────────────────────────────

inline bool isSystemHeader(clang::SourceLocation Loc,
                             const clang::SourceManager& SM) {
    return SM.isInSystemHeader(Loc);
}

// Returns true when Loc1 and Loc2 are in the same translation-unit file.
inline bool inSameFile(clang::SourceLocation Loc1,
                        clang::SourceLocation Loc2,
                        const clang::SourceManager& SM) {
    return SM.getFileID(Loc1) == SM.getFileID(Loc2);
}

} // namespace pp
} // namespace sdc
