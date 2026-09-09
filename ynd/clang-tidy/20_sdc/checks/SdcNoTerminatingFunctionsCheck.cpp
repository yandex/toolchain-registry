#include "SdcNoTerminatingFunctionsCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/StringSwitch.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
namespace {
bool oneOf(StringRef N, std::initializer_list<StringRef> Names) {
    for (auto Candidate : Names) if (Candidate == N) return true;
    return false;
}
} // namespace
void SdcNoTerminatingFunctionsCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(declRefExpr(to(functionDecl())).bind("ref"), this);
}
void SdcNoTerminatingFunctionsCheck::check(const MatchFinder::MatchResult &Result) {
    const auto *E = Result.Nodes.getNodeAs<DeclRefExpr>("ref");
    const auto *F = dyn_cast<FunctionDecl>(E->getDecl());
    if (!F || !F->getIdentifier() ||
        !oneOf(F->getName(), {"abort", "exit", "_Exit", "quick_exit", "terminate"})) return;
    bool Std = F->isInStdNamespace();
    if (!Std && !(F->getDeclContext()->getRedeclContext()->isTranslationUnit() &&
        Result.SourceManager->isInSystemHeader(F->getLocation()))) return;
    if (!F->getReturnType()->isVoidType() || F->isVariadic()) return;
    bool TakesCode = oneOf(F->getName(), {"exit", "_Exit", "quick_exit"});
    if (F->getNumParams() != unsigned(TakesCode) ||
        (TakesCode && !F->getParamDecl(0)->getType()->isSpecificBuiltinType(BuiltinType::Int))) return;
    if (isInAnalyzedCode(*E, E->getLocation(), *Result.Context))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*E), E->getLocation(),
            "do not call or take the address of a program-terminating library function", *Result.Context, Instances);
}
} // namespace clang::tidy::sdc
