#include "SdcDeleteIncompletePtrCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
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

namespace {

SourceLocation getCompletenessPoint(const Decl* Instance,
                                    const CXXDeleteExpr& Delete) {
    if (const auto* FD = dyn_cast_or_null<FunctionDecl>(Instance)) {
        const SourceLocation Point = FD->getPointOfInstantiation();
        if (Point.isValid())
            return Point;
    }
    if (const auto* CTSD =
            dyn_cast_or_null<ClassTemplateSpecializationDecl>(Instance)) {
        const SourceLocation Point = CTSD->getPointOfInstantiation();
        if (Point.isValid())
            return Point;
    }
    if (const auto* VTSD =
            dyn_cast_or_null<VarTemplateSpecializationDecl>(Instance)) {
        const SourceLocation Point = VTSD->getPointOfInstantiation();
        if (Point.isValid())
            return Point;
    }
    return Delete.getBeginLoc();
}

bool isCompleteAt(const RecordDecl& Record, SourceLocation Point,
                  const SourceManager& SM) {
    const RecordDecl* Definition = Record.getDefinition();
    if (!Definition)
        return false;

    Point = SM.getExpansionLoc(Point);
    SourceLocation DefinitionLoc =
        SM.getExpansionLoc(Definition->getBeginLoc());
    if (Point.isInvalid() || DefinitionLoc.isInvalid())
        return false;

    return DefinitionLoc == Point ||
           SM.isBeforeInTranslationUnit(DefinitionLoc, Point);
}

} // namespace

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
    for (const Decl* Instance :
         AnalysisInstances.claim(*DE, DE->getBeginLoc(), *Result.Context)) {
        const SourceLocation Point = getCompletenessPoint(Instance, *DE);
        if (!isCompleteAt(*RD, Point, *Result.SourceManager)) {
            diag(DE->getBeginLoc(),
                 "deleting pointer to incomplete type '%0'")
                << T.getUnqualifiedType();
        }
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
