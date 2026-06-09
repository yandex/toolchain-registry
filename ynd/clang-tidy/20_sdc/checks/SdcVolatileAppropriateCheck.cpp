#include "SdcVolatileAppropriateCheck.h"

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

// Returns true for implicit template instantiations — we only warn on the
// pattern so the user sees one diagnostic instead of one per instantiation.
static bool isImplicitInstantiation(const FunctionDecl* FD) {
    auto TSK = FD->getTemplateSpecializationKind();
    return TSK == TSK_ImplicitInstantiation ||
           TSK == TSK_ExplicitInstantiationDeclaration ||
           TSK == TSK_ExplicitInstantiationDefinition;
}

static bool isImplicitInstantiation(const VarDecl* VD) {
    auto TSK = VD->getTemplateSpecializationKind();
    return TSK == TSK_ImplicitInstantiation ||
           TSK == TSK_ExplicitInstantiationDeclaration ||
           TSK == TSK_ExplicitInstantiationDefinition;
}

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
        if (!VD->isLocalVarDecl()) return;
        // Block-scope extern declarations reference an external global — they
        // are not local variables and volatile on the global is permitted.
        if (VD->hasExternalStorage()) return;
        if (isImplicitInstantiation(VD)) return;
        if (!VD->getType().isVolatileQualified()) return;

        if (isa<DecompositionDecl>(VD))
            diag(VD->getLocation(),
                 "structured binding shall not be declared volatile");
        else
            diag(VD->getLocation(),
                 "local variable %0 shall not be declared volatile")
                << VD;
        return;
    }

    // ── Case 2: function parameter ──────────────────────────────────────────
    if (const auto* PD = Result.Nodes.getNodeAs<ParmVarDecl>("param")) {
        if (!PD->getType().isVolatileQualified()) return;
        // Suppress on instantiations — warn on the template pattern only.
        if (const auto* FD = dyn_cast_or_null<FunctionDecl>(PD->getDeclContext()))
            if (isImplicitInstantiation(FD)) return;

        diag(PD->getLocation(),
             "function parameter %0 shall not be declared volatile")
            << PD;
        return;
    }

    // ── Case 3: free function volatile return type ───────────────────────────
    if (const auto* FD = Result.Nodes.getNodeAs<FunctionDecl>("func")) {
        if (!FD->isFirstDecl()) return;
        if (isImplicitInstantiation(FD)) return;
        if (!FD->getReturnType().isVolatileQualified()) return;

        diag(FD->getLocation(),
             "function %0 shall not have a volatile return type")
            << FD;
        return;
    }

    // ── Case 3 + 4: method volatile return type / volatile qualifier ─────────
    if (const auto* MD = Result.Nodes.getNodeAs<CXXMethodDecl>("method")) {
        if (!MD->isFirstDecl()) return;
        if (isImplicitInstantiation(MD)) return;

        if (MD->getReturnType().isVolatileQualified())
            diag(MD->getLocation(),
                 "method %0 shall not have a volatile return type")
                << MD;

        if (MD->isVolatile())
            diag(MD->getLocation(),
                 "method %0 shall not be declared with a volatile qualifier")
                << MD;
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
