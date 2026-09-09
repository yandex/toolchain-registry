#include "SdcPolicyDiagnostic.h"
#include "SdcNoBitFieldsCheck.h"
#include "SdcCodeSelection.h"

#include "clang/AST/Decl.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcNoBitFieldsCheck::SdcNoBitFieldsCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcNoBitFieldsCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        fieldDecl(isBitField(), unless(isExpansionInSystemHeader()))
            .bind("field"),
        this);
}

void SdcNoBitFieldsCheck::check(
    const MatchFinder::MatchResult& Result) {
    const auto* Field = Result.Nodes.getNodeAs<FieldDecl>("field");
    if (!Field) {
        return;
    }
    const SourceManager& SM = *Result.SourceManager;
    // Anchor the construct rather than its name. In a macro-generated field,
    // the name may be a call-site argument while the bit-field syntax lives in
    // the macro body; the width location points developers at that body.
    const Expr* Width = Field->getBitWidth();
    SourceLocation Anchor = Width ? Width->getBeginLoc()
                                  : Field->getSourceRange().getEnd();
    if (!isInAnalyzedCode(*Field, Anchor, *Result.Context)) {
        return;
    }
    SourceLocation Location = SM.getSpellingLoc(Anchor);

    for (const Decl* Instance :
         AnalysisInstances.claim(*Field, Anchor, *Result.Context)) {

        if (Field->getIdentifier()) {
            diagnoseAnalysisInstance(*this, Instance, *Result.Context, Location, "bit-field '%0' should not be declared", Field->getName());
        } else {
            diagnoseAnalysisInstance(*this, Instance, *Result.Context, Location, "unnamed bit-field should not be declared");
        }
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
