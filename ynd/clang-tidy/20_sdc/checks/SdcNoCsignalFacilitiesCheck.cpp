#include "SdcNoCsignalFacilitiesCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

namespace clang {
namespace tidy {
namespace sdc {

namespace {

// Both global and std:: spellings - per the rule's note, <signal.h> is also
// covered (and exposes the names in the global namespace).
const StringRef ProhibitedCsignalFunctions[] = {
    "::signal", "::raise",
    "::std::signal", "::std::raise",
};

const StringRef ProhibitedCsignalTypes[] = {
    "::sig_atomic_t",
    "::std::sig_atomic_t",
};

const StringRef ProhibitedCsignalMacros[] = {
    "SIG_DFL", "SIG_ERR", "SIG_IGN",
    "SIGABRT", "SIGFPE", "SIGILL", "SIGINT", "SIGSEGV", "SIGTERM",
};

// Check whether the second argument of a `signal()` call was spelled with
// the SIG_IGN macro. The macro expands to something like `((void(*)(int))1)`;
// we check the immediate macro name at the argument's expansion site.
bool secondArgIsSigIgn(const CallExpr* Call, const SourceManager& SM,
                       const LangOptions& LO) {
    if (Call->getNumArgs() != 2) {
        return false;
    }
    SourceLocation Loc = Call->getArg(1)->getBeginLoc();
    if (Loc.isInvalid() || !Loc.isMacroID()) {
        return false;
    }
    return Lexer::getImmediateMacroName(Loc, SM, LO) == "SIG_IGN";
}

} // namespace

SdcNoCsignalFacilitiesCheck::SdcNoCsignalFacilitiesCheck(
    StringRef Name, ClangTidyContext* Context)
    : SdcBannedHeaderFacilitiesCheck(Name, Context) {}

ArrayRef<StringRef> SdcNoCsignalFacilitiesCheck::getProhibitedFunctions() const {
    return ProhibitedCsignalFunctions;
}

ArrayRef<StringRef> SdcNoCsignalFacilitiesCheck::getProhibitedTypes() const {
    return ProhibitedCsignalTypes;
}

ArrayRef<StringRef> SdcNoCsignalFacilitiesCheck::getProhibitedMacros() const {
    return ProhibitedCsignalMacros;
}

StringRef SdcNoCsignalFacilitiesCheck::getHeaderName() const {
    return "<csignal>";
}

bool SdcNoCsignalFacilitiesCheck::isExemptCall(const CallExpr* Call) const {
    // Permitted: signal(X, SIG_IGN). The callee is one of the prohibited
    // functions (the base matched it); narrow to the `signal` name and
    // require the second arg to be the SIG_IGN macro.
    if (!Call) {
        return false;
    }
    const FunctionDecl* FD = Call->getDirectCallee();
    if (!FD) {
        return false;
    }
    StringRef Name = FD->getName();
    if (Name != "signal") {
        return false;
    }
    const SourceManager* SM = getSourceManager();
    if (!SM) {
        return false;
    }
    return secondArgIsSigIgn(Call, *SM,
                             Call->getCalleeDecl()->getASTContext().getLangOpts());
}

} // namespace sdc
} // namespace tidy
} // namespace clang
