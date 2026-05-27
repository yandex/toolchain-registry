#include "SdcBannedMainCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcBannedMainCheck::SdcBannedMainCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcBannedMainCheck::registerMatchers(MatchFinder* Finder) {
    // We ban functions named 'main' UNLESS they are in the global namespace
    // To do this, we match any function named "main" and check its DeclContext.
    Finder->addMatcher(
        functionDecl(hasName("main"), unless(isExpansionInSystemHeader())).bind("func"),
        this);
}

void SdcBannedMainCheck::check(const MatchFinder::MatchResult& Result) {
    const auto* Func = Result.Nodes.getNodeAs<FunctionDecl>("func");
    if (!Func) return;

    // Check if it's the global main function
    if (Func->getDeclContext()->isTranslationUnit()) {
        return; // Exempt: global main is allowed
    }

    // Check if it's extern "C" which also lives in global namespace semantically
    if (Func->isExternC() && Func->getDeclContext()->isTranslationUnit()) {
        return;
    }

    diag(Func->getLocation(), "functions shall not be named 'main' unless they are the global program entry point");
}

} // namespace sdc
} // namespace tidy
} // namespace clang
