#include "SdcPolicyDiagnostic.h"
#include "SdcSingleVariableDeclarationCheck.h"
#include "SdcCodeSelection.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcSingleVariableDeclarationCheck::SdcSingleVariableDeclarationCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcSingleVariableDeclarationCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        declStmt(unless(isExpansionInSystemHeader())).bind("localGroup"), this);
    Finder->addMatcher(
        varDecl(unless(parmVarDecl()), unless(isImplicit()),
                unless(hasParent(declStmt())),
                unless(isExpansionInSystemHeader()))
            .bind("nonLocalVariable"),
        this);
    Finder->addMatcher(
        fieldDecl(unless(isImplicit()), unless(isExpansionInSystemHeader()))
            .bind("field"),
        this);
}

void SdcSingleVariableDeclarationCheck::check(
    const MatchFinder::MatchResult& Result) {
    const SourceManager& SM = *Result.SourceManager;

    if (const auto* Group =
            Result.Nodes.getNodeAs<DeclStmt>("localGroup")) {
        if (!isInAnalyzedCode(*Group, Group->getBeginLoc(),
                              *Result.Context)) {
            return;
        }
        unsigned VariableCount = 0;
        for (const Decl* Declaration : Group->decls()) {
            if (isa<VarDecl>(Declaration) && ++VariableCount > 1) {
                break;
            }
        }
        if (VariableCount < 2) {
            return;
        }

        SourceLocation Location = SM.getSpellingLoc(Group->getBeginLoc());
        for (const Decl* Instance : AnalysisInstances.claim(
                 *Group, Group->getBeginLoc(), *Result.Context)) {

            diagnoseAnalysisInstance(*this, Instance, *Result.Context, Location,
                 "a declaration should not declare more than one variable");
        }
        return;
    }

    const DeclaratorDecl* Declaration =
        Result.Nodes.getNodeAs<VarDecl>("nonLocalVariable");
    if (!Declaration) {
        Declaration = Result.Nodes.getNodeAs<FieldDecl>("field");
    }
    if (!Declaration) {
        return;
    }
    if (!isInAnalyzedCode(*Declaration, Declaration->getBeginLoc(),
                          *Result.Context)) {
        return;
    }

    SourceLocation Begin = Declaration->getBeginLoc();
    SourceLocation SpellingBegin = SM.getSpellingLoc(Begin);
    SourceLocation MemberLocation =
        SM.getSpellingLoc(Declaration->getLocation());
    if (SpellingBegin.isInvalid() || MemberLocation.isInvalid() ||
        SM.isInSystemHeader(SpellingBegin)) {
        return;
    }

    // A macro definition may be expanded into several independent declaration
    // statements. Group by expansion site so those uses are not combined, but
    // point the eventual warning at the macro spelling developers must edit.
    SourceLocation GroupLocation =
        Begin.isMacroID() ? SM.getExpansionLoc(Begin) : SpellingBegin;
    const unsigned GroupKey = GroupLocation.getRawEncoding();
    auto Instances = collectTemplateInstantiationContexts(
        DynTypedNode::create(*Declaration), *Result.Context);
    if (Instances.empty()) Instances.push_back(nullptr);
    for (const Decl *Instance : Instances) {
        auto &Members = GroupMembers[{GroupKey, Instance}];
        // Group and deduplicate independently in each concrete specialization.
        if (!Members.insert(MemberLocation.getRawEncoding()).second ||
            Members.size() != 2) continue;
        diagnoseAnalysisInstance(*this, Instance, *Result.Context, SpellingBegin,
             "a declaration should not declare more than one variable or member "
             "variable");
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
