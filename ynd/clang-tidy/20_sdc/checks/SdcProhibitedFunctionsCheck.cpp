#include "SdcProhibitedFunctionsCheck.h"
#include "SdcCodeSelection.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/StringSet.h"

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

// Diagnose the public macro use, irrespective of its replacement tokens.
// This is a preprocessing restriction: it also covers empty expansions,
// preprocessing expressions and dormant template bodies.
class ProhibitedFunctionMacroCallbacks : public PPCallbacks {
public:
    ProhibitedFunctionMacroCallbacks(SdcProhibitedFunctionsCheck& Check,
                                     const SourceManager& SM)
        : Check(Check), SM(SM) {
        for (StringRef Name : Check.getProhibitedFunctions()) {
            Name = Name.substr(Name.rfind(':') + 1);
            if (!Check.isAllowedMacro(Name))
                Names.insert(Name);
        }
    }

    void MacroExpands(const Token& Token, const MacroDefinition& Definition,
                      SourceRange, const MacroArgs*) override {
        const IdentifierInfo* Identifier = Token.getIdentifierInfo();
        const MacroInfo* Macro = Definition.getMacroInfo();
        if (!Identifier || !Names.contains(Identifier->getName()) || !Macro ||
            !SM.isInSystemHeader(Macro->getDefinitionLoc()))
            return;
        const SourceLocation Loc = Token.getLocation();
        if (!isWrittenInAnalyzedSource(Loc, SM))
            return;
        Check.diag(Loc, Check.getDiagnosticMessage(Identifier->getName()));
    }

private:
    SdcProhibitedFunctionsCheck& Check;
    const SourceManager& SM;
    llvm::StringSet<> Names;
};

void SdcProhibitedFunctionsCheck::registerPPCallbacks(
    const SourceManager& SM, Preprocessor* PP, Preprocessor*) {
    PP->addPPCallbacks(
        std::make_unique<ProhibitedFunctionMacroCallbacks>(*this, SM));
}

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
        if (!isInAnalyzedCode(*DRE, DRE->getBeginLoc(), *Result.Context))
            return;
        const NamedDecl* ND = DRE->getDecl();
        if (!isProhibitedQualifiedName(Functions, ND->getQualifiedNameAsString()))
            return;
        if (const auto* FD = dyn_cast<FunctionDecl>(ND))
            if (isAllowedDecl(FD))
                return;
        ExtractedName = ND->getNameAsString();
        FunctionName = ExtractedName;
        Loc = DRE->getBeginLoc();
    } else if (const auto* ULE = Result.Nodes.getNodeAs<UnresolvedLookupExpr>("unresolvedFuncUse")) {
        if (!isInAnalyzedCode(*ULE, ULE->getBeginLoc(), *Result.Context))
            return;
        Loc = ULE->getBeginLoc();
        for (const NamedDecl* ND : ULE->decls()) {
            if (isProhibitedQualifiedName(Functions, ND->getQualifiedNameAsString())) {
                if (const auto* FD = dyn_cast<FunctionDecl>(ND))
                    if (isAllowedDecl(FD))
                        continue;
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

    const DynTypedNode Node =
        Result.Nodes.getNodeAs<DeclRefExpr>("funcUse")
            ? DynTypedNode::create(
                  *Result.Nodes.getNodeAs<DeclRefExpr>("funcUse"))
            : DynTypedNode::create(
                  *Result.Nodes.getNodeAs<UnresolvedLookupExpr>(
                      "unresolvedFuncUse"));
    for (const Decl* Instance :
         AnalysisInstances.claim(Node, Loc, *Result.Context)) {
        (void)Instance;
        diag(Loc, getDiagnosticMessage(FunctionName));
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
