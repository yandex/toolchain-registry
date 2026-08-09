#include "SdcNoUnscopedEnumCheck.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcNoUnscopedEnumCheck::SdcNoUnscopedEnumCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcNoUnscopedEnumCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        enumDecl(unless(isScoped()), unless(isImplicit()),
                 unless(isExpansionInSystemHeader()))
            .bind("enum"),
        this);
}

void SdcNoUnscopedEnumCheck::check(
    const MatchFinder::MatchResult& Result) {
    const auto* Enumeration = Result.Nodes.getNodeAs<EnumDecl>("enum");
    if (!Enumeration || isa<CXXRecordDecl>(Enumeration->getDeclContext())) {
        return;
    }

    const SourceManager& SM = *Result.SourceManager;
    SourceLocation Location = SM.getSpellingLoc(Enumeration->getBeginLoc());
    if (Location.isInvalid() || SM.isInSystemHeader(Location) ||
        !ReportedLocations.insert(Location.getRawEncoding()).second) {
        return;
    }

    if (Enumeration->getIdentifier()) {
        diag(Location,
             "unscoped enumeration '%0' should not be declared outside a "
             "class or struct")
            << Enumeration->getName();
    } else {
        diag(Location,
             "unnamed unscoped enumeration should not be declared outside a "
             "class or struct");
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
