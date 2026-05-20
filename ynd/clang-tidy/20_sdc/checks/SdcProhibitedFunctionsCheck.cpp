#include "SdcProhibitedFunctionsCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

namespace {
// Strip a single leading "::" so callers can compare qualified names without
// having to remember which side has it. getQualifiedNameAsString() returns
// e.g. "std::strerror" (no prefix); prohibited list entries use "::std::strerror".
StringRef stripGlobalPrefix(StringRef Name) {
    return Name.starts_with("::") ? Name.drop_front(2) : Name;
}

bool isProhibitedQualifiedName(ArrayRef<StringRef> Prohibited, StringRef QualName) {
    StringRef Normalized = stripGlobalPrefix(QualName);
    for (StringRef P : Prohibited) {
        if (stripGlobalPrefix(P) == Normalized)
            return true;
    }
    return false;
}
} // namespace

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
    ArrayRef<StringRef> Functions = getProhibitedFunctions();

    // Defensive filter: the Yandex-patched clang-tidy implements hasName()
    // by unqualified name only and ignores the leading "::" anchor, so the
    // matcher overmatches against any function called e.g. `strerror` in any
    // namespace or class scope. Re-verify the resolved declaration's full
    // qualified name against the prohibited list explicitly.
    if (const auto* DRE = Result.Nodes.getNodeAs<DeclRefExpr>("funcUse")) {
        const NamedDecl* ND = DRE->getDecl();
        if (!isProhibitedQualifiedName(Functions, ND->getQualifiedNameAsString()))
            return;
        ExtractedName = ND->getNameAsString();
        FunctionName = ExtractedName;
        Loc = DRE->getBeginLoc();
    } else if (const auto* ULE = Result.Nodes.getNodeAs<UnresolvedLookupExpr>("unresolvedFuncUse")) {
        Loc = ULE->getBeginLoc();
        for (const NamedDecl* ND : ULE->decls()) {
            if (isProhibitedQualifiedName(Functions, ND->getQualifiedNameAsString())) {
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
