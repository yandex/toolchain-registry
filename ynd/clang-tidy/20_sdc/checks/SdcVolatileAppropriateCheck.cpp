#include "SdcVolatileAppropriateCheck.h"
#include "SdcCodeSelection.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcVolatileAppropriateCheck::SdcVolatileAppropriateCheck(StringRef Name,
                                                          ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcVolatileAppropriateCheck::registerMatchers(MatchFinder* Finder) {
    // Case 1 + 5: volatile local variables and volatile structured bindings.
    // isLocalVarDecl() has no matcher in clang-20 — filter in check().
    Finder->addMatcher(
        varDecl(
            unless(parmVarDecl()),
            unless(isExpansionInSystemHeader())
        ).bind("local"),
        this
    );

    // Case 2: volatile function parameters.
    Finder->addMatcher(
        parmVarDecl(
            unless(isExpansionInSystemHeader())
        ).bind("param"),
        this
    );

    // Case 3: volatile function return type (free functions).
    // Case 4 + 3 for methods handled separately below.
    Finder->addMatcher(
        functionDecl(
            unless(cxxMethodDecl()),
            unless(isExpansionInSystemHeader())
        ).bind("func"),
        this
    );

    // Case 3 + 4: methods — volatile return type and/or volatile qualifier.
    Finder->addMatcher(
        cxxMethodDecl(
            unless(isExpansionInSystemHeader())
        ).bind("method"),
        this
    );
}

void SdcVolatileAppropriateCheck::check(const MatchFinder::MatchResult& Result) {
    // ── Case 1 + 5: local variable (including structured bindings) ──────────
    if (const auto* VD = Result.Nodes.getNodeAs<VarDecl>("local")) {
        if (!isInAnalyzedCode(*VD, VD->getLocation(), *Result.Context)) return;

        // The structured-binding prohibition is independent of storage
        // duration.  Handle it before the ordinary-local-variable filter so a
        // namespace-scope decomposition cannot escape the rule.
        if (isa<DecompositionDecl>(VD)) {
            if (VD->getType().isVolatileQualified()) {
                for (const Decl* Instance : AnalysisInstances.claim(
                         *VD, VD->getLocation(), *Result.Context)) {
                    (void)Instance;
                    diag(VD->getLocation(),
                         "structured binding shall not be declared volatile");
                }
            }
            return;
        }

        if (!VD->isLocalVarDecl()) return;
        // Block-scope extern declarations reference an external global — they
        // are not local variables and volatile on the global is permitted.
        if (VD->hasExternalStorage()) return;
        if (!VD->getType().isVolatileQualified()) return;

        for (const Decl* Instance : AnalysisInstances.claim(
                 *VD, VD->getLocation(), *Result.Context)) {
            (void)Instance;
            diag(VD->getLocation(),
                 "local variable %0 shall not be declared volatile")
                << VD;
        }
        return;
    }

    // ── Case 2: function parameter ──────────────────────────────────────────
    if (const auto* PD = Result.Nodes.getNodeAs<ParmVarDecl>("param")) {
        if (!PD->getType().isVolatileQualified()) return;
        if (!isInAnalyzedCode(*PD, PD->getLocation(), *Result.Context)) return;
        for (const Decl* Instance : AnalysisInstances.claim(
                 *PD, PD->getLocation(), *Result.Context)) {
            (void)Instance;
            diag(PD->getLocation(),
                 "function parameter %0 shall not be declared volatile")
                << PD;
        }
        return;
    }

    // ── Case 3: free function volatile return type ───────────────────────────
    if (const auto* FD = Result.Nodes.getNodeAs<FunctionDecl>("func")) {
        if (!FD->isFirstDecl()) return;
        if (!FD->getReturnType().isVolatileQualified()) return;
        if (!isInAnalyzedCode(*FD, FD->getLocation(), *Result.Context)) return;

        for (const Decl* Instance : AnalysisInstances.claim(
                 *FD, FD->getLocation(), *Result.Context)) {
            (void)Instance;
            diag(FD->getLocation(),
                 "function %0 shall not have a volatile return type")
                << FD;
        }
        return;
    }

    // ── Case 3 + 4: method volatile return type / volatile qualifier ─────────
    if (const auto* MD = Result.Nodes.getNodeAs<CXXMethodDecl>("method")) {
        if (!MD->isFirstDecl()) return;
        if (!isInAnalyzedCode(*MD, MD->getLocation(), *Result.Context)) return;

        const bool BadReturn = MD->getReturnType().isVolatileQualified();
        const bool BadQualifier = MD->isVolatile();
        if (!BadReturn && !BadQualifier) return;
        for (const Decl* Instance : AnalysisInstances.claim(
                 *MD, MD->getLocation(), *Result.Context)) {
            (void)Instance;
            if (BadReturn) {
                diag(MD->getLocation(),
                     "method %0 shall not have a volatile return type")
                    << MD;
            }
            if (BadQualifier) {
                diag(MD->getLocation(),
                     "method %0 shall not be declared with a volatile qualifier")
                    << MD;
            }
        }
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
