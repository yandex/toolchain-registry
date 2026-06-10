#include "SdcUnscopedEnumNumericUseCheck.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

namespace {

// Returns the EnumDecl if the type is an unscoped enum without an explicitly
// written underlying type (no enum-base in source), otherwise null.
const EnumDecl* unscopedUnfixed(QualType Q) {
    const auto* ET = Q.getCanonicalType()->getAs<EnumType>();
    if (!ET) return nullptr;
    const EnumDecl* ED = ET->getDecl();
    if (ED->isScoped()) return nullptr;
    if (ED->getIntegerTypeSourceInfo() != nullptr) return nullptr;
    return ED;
}

// Returns the EnumDecl if the expression, after stripping implicit casts,
// has an unscoped-unfixed enum type.
const EnumDecl* unscopedUnfixedOf(const Expr* E) {
    if (!E) return nullptr;
    const Expr* Inner = E->IgnoreImpCasts();
    QualType T = Inner->getType();
    // ParenListExpr and dependent expressions in template patterns have null or
    // dependent types that crash getCanonicalType() — guard before use.
    if (T.isNull() || T->isDependentType()) return nullptr;
    return unscopedUnfixed(T);
}

// Returns true if Target can hold all values of ED without losing information.
// We compare bit-widths of Target against Clang's chosen underlying type for ED.
bool isLargeEnough(QualType Target, const EnumDecl* ED, ASTContext& Ctx) {
    QualType Underlying = ED->getIntegerType();
    if (Underlying.isNull()) return true;
    Target = Ctx.getCanonicalType(Target);
    if (!Target->isIntegerType()) return false;
    return Ctx.getTypeSize(Target) >= Ctx.getTypeSize(Underlying);
}

// Returns true if the expression is inside an unevaluated operand
// (sizeof, alignof, decltype, noexcept) — the rule does not apply there.
bool isUnevaluated(const Expr* E, ASTContext& Ctx) {
    DynTypedNodeList Parents = Ctx.getParents(*E);
    while (!Parents.empty()) {
        const DynTypedNode& P = Parents[0];
        if (const auto* T = P.get<UnaryExprOrTypeTraitExpr>()) {
            auto K = T->getKind();
            if (K == UETT_SizeOf || K == UETT_AlignOf)
                return true;
        }
        if (P.get<CXXNoexceptExpr>()) return true;
        if (P.get<DecltypeType>())    return true;
        Parents = Ctx.getParents(P);
    }
    return false;
}

} // namespace

SdcUnscopedEnumNumericUseCheck::SdcUnscopedEnumNumericUseCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcUnscopedEnumNumericUseCheck::registerMatchers(MatchFinder* Finder) {
    const auto noSysHeader = unless(isExpansionInSystemHeader());

    // Arithmetic, bitwise, shift, logical, and compound-assignment operators.
    Finder->addMatcher(
        binaryOperator(noSysHeader, anyOf(
            hasOperatorName("+"),   hasOperatorName("-"),
            hasOperatorName("*"),   hasOperatorName("/"),  hasOperatorName("%"),
            hasOperatorName("&"),   hasOperatorName("|"),  hasOperatorName("^"),
            hasOperatorName("<<"),  hasOperatorName(">>"),
            hasOperatorName("&&"),  hasOperatorName("||"),
            hasOperatorName("+="),  hasOperatorName("-="),
            hasOperatorName("*="),  hasOperatorName("/="), hasOperatorName("%="),
            hasOperatorName("&="),  hasOperatorName("|="), hasOperatorName("^="),
            hasOperatorName("<<="), hasOperatorName(">>=")
        )).bind("arith"),
        this);

    // Unary arithmetic, bitwise, and logical operators.
    Finder->addMatcher(
        unaryOperator(noSysHeader, anyOf(
            hasOperatorName("+"), hasOperatorName("-"),
            hasOperatorName("~"), hasOperatorName("!")
        )).bind("unary"),
        this);

    // Relational and equality operators.
    Finder->addMatcher(
        binaryOperator(noSysHeader, anyOf(
            hasOperatorName("=="), hasOperatorName("!="),
            hasOperatorName("<"),  hasOperatorName("<="),
            hasOperatorName(">"),  hasOperatorName(">=")
        )).bind("cmp"),
        this);

    // static_cast — covers both "to" and "from" unscoped-unfixed enum.
    Finder->addMatcher(
        cxxStaticCastExpr(noSysHeader).bind("scast"),
        this);

    // switch statement.
    Finder->addMatcher(
        switchStmt(noSysHeader).bind("sw"),
        this);

    // Implicit integral conversion — catches assignments, function arguments,
    // and return statements where the target integer is too narrow.
    Finder->addMatcher(
        implicitCastExpr(noSysHeader, hasCastKind(CK_IntegralCast)).bind("impl"),
        this);
}

void SdcUnscopedEnumNumericUseCheck::check(
    const MatchFinder::MatchResult& Result) {

    ASTContext& Ctx = *Result.Context;

    // --- Arithmetic / bitwise / shift / logical / compound-assignment ---
    if (const auto* BO = Result.Nodes.getNodeAs<BinaryOperator>("arith")) {
        if (isUnevaluated(BO, Ctx)) return;
        if (!unscopedUnfixedOf(BO->getLHS()) && !unscopedUnfixedOf(BO->getRHS()))
            return;
        diag(BO->getOperatorLoc(),
             "unscoped enumeration with no fixed underlying type shall not be "
             "used as an operand to '%0'")
            << BO->getOpcodeStr();
        return;
    }

    // --- Unary +  -  ~  ! ---
    if (const auto* UO = Result.Nodes.getNodeAs<UnaryOperator>("unary")) {
        if (isUnevaluated(UO, Ctx)) return;
        if (!unscopedUnfixedOf(UO->getSubExpr())) return;
        diag(UO->getOperatorLoc(),
             "unscoped enumeration with no fixed underlying type shall not be "
             "used as an operand to '%0'")
            << UnaryOperator::getOpcodeStr(UO->getOpcode());
        return;
    }

    // --- Relational / equality ---
    if (const auto* BO = Result.Nodes.getNodeAs<BinaryOperator>("cmp")) {
        if (isUnevaluated(BO, Ctx)) return;
        const EnumDecl* L = unscopedUnfixedOf(BO->getLHS());
        const EnumDecl* R = unscopedUnfixedOf(BO->getRHS());
        if (!L && !R) return;
        // Compliant only when both operands share the same unscoped-unfixed enum.
        if (L && R && L->getCanonicalDecl() == R->getCanonicalDecl()) return;
        diag(BO->getOperatorLoc(),
             "unscoped enumeration with no fixed underlying type shall not be "
             "compared with '%0' unless both operands have the same enumeration type")
            << BO->getOpcodeStr();
        return;
    }

    // --- static_cast ---
    if (const auto* SC = Result.Nodes.getNodeAs<CXXStaticCastExpr>("scast")) {
        if (isUnevaluated(SC, Ctx)) return;
        QualType RawTarget = SC->getType();
        if (RawTarget.isNull() || RawTarget->isDependentType()) return;
        QualType Target = RawTarget.getCanonicalType();

        // static_cast TO an unscoped-unfixed enum is always prohibited.
        if (unscopedUnfixed(Target)) {
            diag(SC->getOperatorLoc(),
                 "static_cast to an unscoped enumeration type with no fixed "
                 "underlying type is prohibited");
            return;
        }

        // static_cast FROM an unscoped-unfixed enum.
        const EnumDecl* Src = unscopedUnfixedOf(SC->getSubExpr());
        if (!Src) return;
        // Allowed: target is same unscoped-unfixed enum type.
        if (unscopedUnfixed(Target) &&
            unscopedUnfixed(Target)->getCanonicalDecl() == Src->getCanonicalDecl())
            return;
        // Allowed: target is a large-enough integer type.
        if (Target->isIntegerType() && isLargeEnough(Target, Src, Ctx)) return;

        diag(SC->getOperatorLoc(),
             "static_cast from unscoped enumeration with no fixed underlying "
             "type to '%0' is prohibited; target is not the same enumeration "
             "type or a sufficiently large integer")
            << SC->getType();
        return;
    }

    // --- switch condition ---
    if (const auto* SS = Result.Nodes.getNodeAs<SwitchStmt>("sw")) {
        const EnumDecl* CondEnum = unscopedUnfixedOf(SS->getCond());
        if (!CondEnum) return;
        for (const SwitchCase* SC = SS->getSwitchCaseList(); SC;
             SC = SC->getNextSwitchCase()) {
            const auto* CS = dyn_cast<CaseStmt>(SC);
            if (!CS) continue; // default: is fine
            const EnumDecl* CaseEnum = unscopedUnfixedOf(CS->getLHS());
            if (!CaseEnum ||
                CaseEnum->getCanonicalDecl() != CondEnum->getCanonicalDecl()) {
                diag(CS->getCaseLoc(),
                     "case constant is not an enumerator of the switch "
                     "condition's unscoped enumeration type with no fixed "
                     "underlying type");
            }
        }
        return;
    }

    // --- Implicit conversion to integer (assignment / argument / return) ---
    if (const auto* IC = Result.Nodes.getNodeAs<ImplicitCastExpr>("impl")) {
        // Implicit casts synthesised as part of an explicit cast are already
        // handled by the static_cast checker — skip them here.
        if (IC->isPartOfExplicitCast()) return;
        if (isUnevaluated(IC, Ctx)) return;
        const EnumDecl* Src = unscopedUnfixedOf(IC->getSubExpr());
        if (!Src) return;
        QualType Target = Ctx.getCanonicalType(IC->getType());
        // Same enum type: not an integral cast scenario, but guard anyway.
        if (unscopedUnfixed(Target)) return;
        // Large-enough integer: compliant.
        if (Target->isIntegerType() && isLargeEnough(Target, Src, Ctx)) return;
        // Also: bool is large enough for any flag-style enum (0/1 range),
        // but the underlying type check handles this — if E has int underlying
        // type and target is bool (1 bit), it will be flagged.
        diag(IC->getExprLoc(),
             "implicit conversion of unscoped enumeration with no fixed "
             "underlying type to '%0', which may not be large enough to "
             "represent all enumerator values")
            << IC->getType();
        return;
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
