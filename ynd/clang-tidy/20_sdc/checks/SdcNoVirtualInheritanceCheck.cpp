#include "SdcNoVirtualInheritanceCheck.h"

#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcNoVirtualInheritanceCheck::SdcNoVirtualInheritanceCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcNoVirtualInheritanceCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        cxxRecordDecl(isDefinition(), unless(isImplicit()),
                      unless(isExpansionInSystemHeader()))
            .bind("record"),
        this);
}

void SdcNoVirtualInheritanceCheck::check(
    const MatchFinder::MatchResult& Result) {
    const auto* Record = Result.Nodes.getNodeAs<CXXRecordDecl>("record");
    if (!Record ||
        Record->getTemplateSpecializationKind() == TSK_ImplicitInstantiation) {
        return;
    }

    const SourceManager& SM = *Result.SourceManager;
    for (const CXXBaseSpecifier& Base : Record->bases()) {
        if (!Base.isVirtual()) {
            continue;
        }

        SourceLocation Location = SM.getSpellingLoc(Base.getBeginLoc());
        if (Location.isInvalid() || SM.isInSystemHeader(Location) ||
            !ReportedLocations.insert(Location.getRawEncoding()).second) {
            continue;
        }

        const CXXRecordDecl* BaseRecord = Base.getType()->getAsCXXRecordDecl();
        StringRef BaseName = BaseRecord ? BaseRecord->getName() : StringRef();
        if (BaseName.empty()) {
            diag(Location, "class '%0' should not use virtual inheritance")
                << Record->getName();
        } else {
            diag(Location, "class '%0' should not inherit virtually from '%1'")
                << Record->getName() << BaseName;
        }
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
