#include "SdcProhibitedFunctionsCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcProhibitedFunctionsCheck::SdcProhibitedFunctionsCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context)
{
}

void SdcProhibitedFunctionsCheck::registerMatchers(MatchFinder* Finder) {
    ArrayRef<StringRef> Functions = getProhibitedFunctions();
    if (Functions.empty()) {
        return;
    }

    // Match any direct use of the prohibited functions
    Finder->addMatcher(
        declRefExpr(
            to(functionDecl(hasAnyName(Functions))),
            unless(isExpansionInSystemHeader()))
            .bind("funcUse"),
        this);

    // Match unresolved uses (e.g., templates, namespace std unresolved)
    Finder->addMatcher(
        unresolvedLookupExpr(
            hasAnyDeclaration(namedDecl(hasAnyName(Functions))),
            unless(isExpansionInSystemHeader()))
            .bind("unresolvedFuncUse"),
        this);
}

void SdcProhibitedFunctionsCheck::check(const MatchFinder::MatchResult& Result) {
    StringRef FunctionName;
    SourceLocation Loc;
    std::string ExtractedName;

    if (const auto* DRE = Result.Nodes.getNodeAs<DeclRefExpr>("funcUse")) {
        const NamedDecl* ND = DRE->getDecl();
        ExtractedName = ND->getNameAsString();
        FunctionName = ExtractedName;
        Loc = DRE->getBeginLoc();
    } else if (const auto* ULE = Result.Nodes.getNodeAs<UnresolvedLookupExpr>("unresolvedFuncUse")) {
        Loc = ULE->getBeginLoc();
        ArrayRef<StringRef> Functions = getProhibitedFunctions();
        for (const NamedDecl* ND : ULE->decls()) {
            std::string QualName = ND->getQualifiedNameAsString();
            std::string QualNameWithColons = "::" + QualName;

            // Check if this declaration's name is in our prohibited list
            bool Found = false;
            for (StringRef Prohibited : Functions) {
                if (QualName == Prohibited || QualNameWithColons == Prohibited || ND->getName() == Prohibited) {
                    Found = true;
                    break;
                }
            }
            if (Found) {
                ExtractedName = ND->getNameAsString();
                FunctionName = ExtractedName;
                break;
            }
        }
        if (FunctionName.empty()) {
            return;
        }
    } else {
        return;
    }

    diag(Loc, getDiagnosticMessage(FunctionName));
}

} // namespace sdc
} // namespace tidy
} // namespace clang
