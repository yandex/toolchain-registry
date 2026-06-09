#include "SdcForwardingReferenceCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcForwardingReferenceCheck::SdcForwardingReferenceCheck(StringRef Name,
                                                          ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

// ─── Helpers ────────────────────────────────────────────────────────────────

// Returns true for a QualType that is a forwarding reference (T&& / auto&&).
static bool hasForwardingRefType(QualType T) {
    if (!T->isRValueReferenceType()) return false;
    QualType Pointee = T.getNonReferenceType();
    return Pointee->getAs<TemplateTypeParmType>() != nullptr ||
           Pointee->getAs<AutoType>() != nullptr;
}

// Returns true if P is a forwarding reference parameter.  Handles both:
//   - Uninstantiated patterns: P's own type is T&& or auto&&
//   - Instantiated code:       P's type is concrete; check the pattern param
static bool isForwardingRef(const ParmVarDecl* P) {
    if (hasForwardingRefType(P->getType())) return true;

    // In a template instantiation the parameter type is substituted.
    // Walk to the primary template to check the original parameter type.
    const auto* FD = dyn_cast<FunctionDecl>(P->getDeclContext());
    if (!FD) return false;
    const FunctionTemplateDecl* FTD = FD->getPrimaryTemplate();
    if (!FTD) return false;
    unsigned Idx = P->getFunctionScopeIndex();
    const FunctionDecl* Pattern = FTD->getTemplatedDecl();
    if (Idx >= Pattern->getNumParams()) return false;
    const ParmVarDecl* PatP = Pattern->getParamDecl(Idx);
    return PatP != P && hasForwardingRefType(PatP->getType());
}

// Returns true if CE is a call to std::forward.
// Handles resolved (DeclRefExpr) and dependent (UnresolvedLookupExpr) callees.
static bool isStdForward(const CallExpr* CE) {
    const Expr* Callee = CE->getCallee()->IgnoreParenCasts();

    auto inStdNS = [](const NamedDecl* D) -> bool {
        if (!D) return false;
        const auto* NS = dyn_cast<NamespaceDecl>(D->getDeclContext());
        return NS && NS->isStdNamespace();
    };

    if (const auto* DRE = dyn_cast<DeclRefExpr>(Callee)) {
        const NamedDecl* D = DRE->getDecl();
        if (!D || D->getName() != "forward") return false;
        return inStdNS(D);
    }
    if (const auto* ULE = dyn_cast<UnresolvedLookupExpr>(Callee)) {
        if (ULE->getName().getAsString() != "forward") return false;
        for (const NamedDecl* D : ULE->decls())
            if (inStdNS(D)) return true;
    }
    return false;
}

// Returns the template argument T from a std::forward<T>(...) call.
static QualType getForwardTemplateArg(const CallExpr* CE) {
    const Expr* Callee = CE->getCallee()->IgnoreParenCasts();
    if (const auto* DRE = dyn_cast<DeclRefExpr>(Callee)) {
        if (DRE->getNumTemplateArgs() > 0)
            return DRE->getTemplateArgs()[0].getArgument().getAsType();
    }
    if (const auto* ULE = dyn_cast<UnresolvedLookupExpr>(Callee)) {
        if (ULE->hasExplicitTemplateArgs() && ULE->getNumTemplateArgs() > 0)
            return ULE->getTemplateArgs()[0].getArgument().getAsType();
    }
    return {};
}

// Returns true if ForwardT is decltype(P).
static bool isDecltypeOfParam(QualType ForwardT, const ParmVarDecl* P) {
    const auto* DT = ForwardT->getAs<DecltypeType>();
    if (!DT) return false;
    const auto* DRE = dyn_cast<DeclRefExpr>(DT->getUnderlyingExpr());
    return DRE && DRE->getDecl() == P;
}

// Returns true when std::forward<ForwardT>(...) correctly forwards parameter P.
// Covers:
//   - Uninstantiated pattern:  ForwardT is the exact TemplateTypeParmType of P
//   - auto&& pattern:          ForwardT is decltype(P)
//   - Instantiated code:       ForwardT matches the concrete type of P (rvalue)
//                              or P's type (lvalue), or decltype(P)
static bool isCorrectForwardType(QualType ForwardT, const ParmVarDecl* P) {
    QualType ParamT    = P->getType();
    QualType ParamNonRef = ParamT.getNonReferenceType();

    // ── Pattern (uninstantiated): T&& or auto&&  ──────────────────────────
    if (const auto* TTPT = ParamNonRef->getAs<TemplateTypeParmType>()) {
        // std::forward<T>  — canonical form for T&&
        if (const auto* FwdTTPT = ForwardT->getAs<TemplateTypeParmType>())
            if (FwdTTPT->getDecl() == TTPT->getDecl()) return true;
        // std::forward<decltype(p)>  — acceptable alternative
        return isDecltypeOfParam(ForwardT, P);
    }
    if (ParamNonRef->getAs<AutoType>())
        return isDecltypeOfParam(ForwardT, P);

    // ── Instantiation: concrete types ────────────────────────────────────
    // T deduced as U (rvalue): P has type U&&, correct forward is std::forward<U>
    // T deduced as U& (lvalue): P has type U&,  correct forward is std::forward<U&>
    QualType FwdCanon   = ForwardT.getCanonicalType();
    if (FwdCanon == ParamNonRef.getCanonicalType()) return true; // rvalue case
    if (FwdCanon == ParamT.getCanonicalType())      return true; // lvalue case
    return isDecltypeOfParam(ForwardT, P);
}

// ─── Part B: validate std::forward call ────────────────────────────────────

static void checkForwardCall(const CallExpr* CE, ClangTidyCheck* Check) {
    if (CE->getNumArgs() == 0) return;

    QualType ForwardT = getForwardTemplateArg(CE);
    if (ForwardT.isNull()) return;

    const Expr* Arg = CE->getArg(0)->IgnoreParenImpCasts();
    const auto* ArgDRE = dyn_cast<DeclRefExpr>(Arg);

    if (!ArgDRE) {
        Check->diag(CE->getBeginLoc(),
                    "std::forward shall only be used to forward a forwarding reference");
        return;
    }

    const auto* P = dyn_cast<ParmVarDecl>(ArgDRE->getDecl());
    if (!P || !isForwardingRef(P)) {
        Check->diag(CE->getBeginLoc(),
                    "std::forward shall only be used to forward a forwarding reference; "
                    "%0 is not a forwarding reference parameter")
            << ArgDRE->getDecl();
        return;
    }

    if (!isCorrectForwardType(ForwardT, P))
        Check->diag(CE->getBeginLoc(),
                    "std::forward template argument does not match the type of "
                    "forwarding reference parameter %0")
            << P;
}

// ─── Part A: check each argument of a non-forward call ─────────────────────

static void checkCallArguments(const CallExpr* CE, ClangTidyCheck* Check) {
    for (unsigned I = 0; I < CE->getNumArgs(); ++I) {
        const Expr* Arg = CE->getArg(I)->IgnoreParenImpCasts();
        const auto* DRE = dyn_cast<DeclRefExpr>(Arg);
        if (!DRE) continue;

        const auto* P = dyn_cast<ParmVarDecl>(DRE->getDecl());
        if (!P || !isForwardingRef(P)) continue;

        Check->diag(DRE->getBeginLoc(),
                    "forwarding reference parameter %0 shall be passed using "
                    "std::forward")
            << P;
    }
}

// ─── Matcher & dispatcher ───────────────────────────────────────────────────

void SdcForwardingReferenceCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        callExpr(unless(isExpansionInSystemHeader())).bind("call"),
        this
    );
}

void SdcForwardingReferenceCheck::check(const MatchFinder::MatchResult& Result) {
    const auto* CE = Result.Nodes.getNodeAs<CallExpr>("call");

    if (isStdForward(CE))
        checkForwardCall(CE, this);
    else
        checkCallArguments(CE, this);
}

} // namespace sdc
} // namespace tidy
} // namespace clang
