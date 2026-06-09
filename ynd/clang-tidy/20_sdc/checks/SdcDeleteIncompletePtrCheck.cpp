#include "SdcDeleteIncompletePtrCheck.h"

#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/SourceManager.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcDeleteIncompletePtrCheck::SdcDeleteIncompletePtrCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcDeleteIncompletePtrCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        cxxDeleteExpr(
            unless(isExpansionInSystemHeader())
        ).bind("del"),
        this
    );
}

void SdcDeleteIncompletePtrCheck::check(const MatchFinder::MatchResult& Result) {
    const auto* DE = Result.Nodes.getNodeAs<CXXDeleteExpr>("del");

    QualType T = DE->getDestroyedType();
    // Dependent types are resolved at instantiation — guard to avoid false
    // positives on uninstantiated template bodies.
    if (T.isNull() || T->isDependentType()) return;

    const auto* RT = T->getAs<RecordType>();
    if (!RT) return;  // scalar, enum, or other non-class type — not our rule

    const RecordDecl* RD = RT->getDecl();
    const RecordDecl* Def = RD->getDefinition();

    bool incomplete;
    if (Def == nullptr) {
        // No definition anywhere in this TU.
        incomplete = true;
    } else {
        // clang-tidy sees the fully-built AST, so even a type defined *after*
        // the delete site appears complete.  Use source locations to detect the
        // "defined later" case: the delete precedes the definition in the file.
        incomplete = Result.SourceManager->isBeforeInTranslationUnit(
            DE->getBeginLoc(), Def->getBeginLoc());
    }

    if (incomplete)
        diag(DE->getBeginLoc(),
             "deleting pointer to incomplete type '%0'")
            << T.getUnqualifiedType();
}

} // namespace sdc
} // namespace tidy
} // namespace clang
