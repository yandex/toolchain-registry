#include "SdcNoAsmCheck.h"

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

    diag(Asm->getAsmLoc(),
         "use of 'asm' is prohibited; use compiler intrinsics instead if low-level "
         "hardware access is required");
}

} // namespace sdc
} // namespace tidy
} // namespace clang
