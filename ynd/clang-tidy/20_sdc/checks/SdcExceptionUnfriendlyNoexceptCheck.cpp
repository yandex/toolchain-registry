#include "SdcExceptionUnfriendlyNoexceptCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcExceptionUnfriendlyNoexceptCheck::SdcExceptionUnfriendlyNoexceptCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcExceptionUnfriendlyNoexceptCheck::registerMatchers(MatchFinder* Finder) {
    const auto noSys = unless(isExpansionInSystemHeader());

    // Sub-clause 2: destructors.
    Finder->addMatcher(
        cxxDestructorDecl(isDefinition(), noSys).bind("dtor"), this);

    // Sub-clause 3: copy constructors of exception classes.
    Finder->addMatcher(
        cxxConstructorDecl(
            isCopyConstructor(), isDefinition(), noSys,
            ofClass(isSameOrDerivedFrom("std::exception"))
        ).bind("exc_copy"), this);

    // Sub-clause 4: move constructors.
    Finder->addMatcher(
        cxxConstructorDecl(isMoveConstructor(), isDefinition(), noSys).bind("move_ctor"),
        this);

    // Sub-clause 5: move assignment operators.
    Finder->addMatcher(
        cxxMethodDecl(isMoveAssignmentOperator(), isDefinition(), noSys).bind("move_assign"),
        this);

    // Sub-clause 6: functions named "swap".
    Finder->addMatcher(
        functionDecl(hasName("swap"), isDefinition(), noSys).bind("swap"),
        this);

    // Sub-clause 1: non-local, non-constexpr variables with static or thread
    // storage that are initialized by a constructor or function call.
    // Local-var filtering is done in the check() handler.
    Finder->addMatcher(
        varDecl(
            noSys,
            unless(isConstexpr()),
            anyOf(hasStaticStorageDuration(), hasThreadStorageDuration()),
            hasType(cxxRecordDecl())
        ).bind("static_var"), this);
}

namespace {

// Returns true when the function is effectively noexcept (cannot throw).
// Returns true for = delete (exempt) and for template-dependent specs.
bool isEffectivelyNoexcept(const FunctionDecl* FD) {
    if (!FD) return true;
    if (FD->isDeleted()) return true; // = delete is exempt per the rule
    if (FD->isImplicit() && isa<CXXDestructorDecl>(FD)) {
        // Implicitly-declared destructors are noexcept(true) by default in C++11+
        return true;
    }
    const auto* FPT = FD->getType()->getAs<FunctionProtoType>();
    if (!FPT) return true;
    auto SpecType = FPT->getExceptionSpecType();
    // Template-dependent or uninstantiated specs: assume OK (can't determine).
    if (SpecType == EST_DependentNoexcept || SpecType == EST_Unevaluated ||
        SpecType == EST_Uninstantiated)
        return true;
    return FPT->isNothrow(/*ResultIfDependent=*/true);
}

// Returns true if the CXXRecordDecl is "std::exception" by name check.
bool isStdExceptionClass(const CXXRecordDecl* RD) {
    if (!RD) return false;
    if (RD->getName() != "exception") return false;
    const DeclContext* DC = RD->getDeclContext();
    if (const auto* NS = dyn_cast_or_null<NamespaceDecl>(DC))
        return NS->isStdNamespace();
    return false;
}

// Returns true when RD inherits from std::exception (or IS std::exception).
bool inheritsFromStdException(const CXXRecordDecl* RD) {
    if (!RD) return false;
    if (isStdExceptionClass(RD)) return true;
    for (const auto& Base : RD->bases()) {
        if (const auto* BaseRD = Base.getType()->getAsCXXRecordDecl())
            if (inheritsFromStdException(BaseRD)) return true;
    }
    return false;
}

} // namespace

void SdcExceptionUnfriendlyNoexceptCheck::check(
    const MatchFinder::MatchResult& Result) {

    // Sub-clause 2: destructor.
    if (const auto* Dtor = Result.Nodes.getNodeAs<CXXDestructorDecl>("dtor")) {
        if (!isEffectivelyNoexcept(Dtor)) {
            diag(Dtor->getLocation(),
                 "destructor '%0' shall be noexcept")
                << Dtor->getQualifiedNameAsString();
        }
        return;
    }

    // Sub-clause 3: copy constructor of an exception class.
    if (const auto* CC = Result.Nodes.getNodeAs<CXXConstructorDecl>("exc_copy")) {
        if (!isEffectivelyNoexcept(CC)) {
            diag(CC->getLocation(),
                 "copy constructor of exception class '%0' shall be noexcept")
                << CC->getParent()->getQualifiedNameAsString();
        }
        return;
    }

    // Sub-clause 4: move constructor.
    if (const auto* MC = Result.Nodes.getNodeAs<CXXConstructorDecl>("move_ctor")) {
        if (!isEffectivelyNoexcept(MC)) {
            diag(MC->getLocation(),
                 "move constructor '%0' shall be noexcept")
                << MC->getQualifiedNameAsString();
        }
        return;
    }

    // Sub-clause 5: move assignment operator.
    if (const auto* MA = Result.Nodes.getNodeAs<CXXMethodDecl>("move_assign")) {
        if (!isEffectivelyNoexcept(MA)) {
            diag(MA->getLocation(),
                 "move assignment operator '%0' shall be noexcept")
                << MA->getQualifiedNameAsString();
        }
        return;
    }

    // Sub-clause 6: function named "swap".
    if (const auto* SW = Result.Nodes.getNodeAs<FunctionDecl>("swap")) {
        if (!isEffectivelyNoexcept(SW)) {
            diag(SW->getLocation(),
                 "function '%0' named 'swap' shall be noexcept")
                << SW->getQualifiedNameAsString();
        }
        return;
    }

    // Sub-clause 1: non-local static/thread variable with class-type initializer.
    if (const auto* VD = Result.Nodes.getNodeAs<VarDecl>("static_var")) {
        // Only non-local variables (function-scope static is still local).
        if (VD->isLocalVarDecl()) return;

        // Find the constructor used to initialize this variable.
        const CXXConstructorDecl* Ctor = nullptr;

        if (const Expr* Init = VD->getInit()) {
            // Strip any implicit casts or cleanup expressions.
            const Expr* E = Init->IgnoreImpCasts();
            if (const auto* EWC = dyn_cast<ExprWithCleanups>(E))
                E = EWC->getSubExpr()->IgnoreImpCasts();
            if (const auto* CE = dyn_cast<CXXConstructExpr>(E))
                Ctor = CE->getConstructor();
        } else {
            // Default-initialization: iterate ctors to find the default one.
            if (const auto* RD = VD->getType()->getAsCXXRecordDecl()) {
                for (const CXXConstructorDecl* C : RD->ctors()) {
                    if (C->isDefaultConstructor() && !C->isImplicit()) {
                        Ctor = C;
                        break;
                    }
                }
                // If no user-provided default ctor, implicit is noexcept by default.
            }
        }

        if (Ctor && !isEffectivelyNoexcept(Ctor)) {
            diag(VD->getLocation(),
                 "constructor used to initialize non-local variable '%0' "
                 "with static or thread storage duration shall be noexcept")
                << VD->getName();
        }
        return;
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
