#include "SdcPolicyDiagnostic.h"
#include "SdcNoAsmCheck.h"
#include "SdcCodeSelection.h"

#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcNoAsmCheck::SdcNoAsmCheck(StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcNoAsmCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        asmStmt(unless(isExpansionInSystemHeader())).bind("asm"),
        this);
}

void SdcNoAsmCheck::check(const MatchFinder::MatchResult& Result) {
    const auto* Asm = Result.Nodes.getNodeAs<AsmStmt>("asm");
    if (!Asm)
        return;
    if (!isInAnalyzedCode(*Asm, Asm->getAsmLoc(), *Result.Context))
        return;

    for (const Decl* Instance :
         AnalysisInstances.claim(*Asm, Asm->getAsmLoc(), *Result.Context)) {

        diagnoseAnalysisInstance(*this, Instance, *Result.Context, Asm->getAsmLoc(),
             "use of 'asm' is prohibited; use compiler intrinsics instead if "
             "low-level hardware access is required");
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
