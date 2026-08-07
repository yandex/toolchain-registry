#include "SdcExceptionUnfriendlyNoexceptCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Type.h"
#include "clang/AST/RecursiveASTVisitor.h"
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
            anyOf(hasStaticStorageDuration(), hasThreadStorageDuration())
        ).bind("static_var"), this);

    // Functions passed across a C-language boundary and registered exit or
    // terminate handlers are also exception-unfriendly contexts.
    Finder->addMatcher(callExpr(noSys).bind("special_call"), this);
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

class ThrowingInitializerVisitor
    : public RecursiveASTVisitor<ThrowingInitializerVisitor> {
public:
    const FunctionDecl* Throwing = nullptr;

    bool VisitCXXConstructExpr(CXXConstructExpr* E) {
        if (!Throwing && !isEffectivelyNoexcept(E->getConstructor()))
            Throwing = E->getConstructor();
        return !Throwing;
    }

    bool VisitCallExpr(CallExpr* E) {
        if (!Throwing)
            if (const FunctionDecl* FD = E->getDirectCallee())
                if (!isEffectivelyNoexcept(FD)) Throwing = FD;
        return !Throwing;
    }
};

class LambdaFinder : public RecursiveASTVisitor<LambdaFinder> {
public:
    const LambdaExpr* Found = nullptr;
    bool VisitLambdaExpr(LambdaExpr* E) {
        if (!Found) Found = E;
        return false;
    }
};

const FunctionDecl* callableFunction(const Expr* E) {
    if (!E) return nullptr;
    E = E->IgnoreParenImpCasts();
    if (const auto* DRE = dyn_cast<DeclRefExpr>(E))
        return dyn_cast<FunctionDecl>(DRE->getDecl());
    if (const auto* LE = dyn_cast<LambdaExpr>(E))
        return LE->getCallOperator();
    // Conversion of a captureless lambda to a function pointer introduces a
    // user-defined conversion call around the LambdaExpr.  Recover the source
    // lambda so its operator() exception specification is checked.
    LambdaFinder Finder;
    Finder.TraverseStmt(const_cast<Expr*>(E));
    if (Finder.Found) return Finder.Found->getCallOperator();
    return nullptr;
}

bool isExitOrTerminateRegistration(const FunctionDecl* FD) {
    if (!FD) return false;
    StringRef Name = FD->getName();
    return Name == "atexit" || Name == "at_quick_exit" ||
           Name == "set_terminate";
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

    if (const auto* Call = Result.Nodes.getNodeAs<CallExpr>("special_call")) {
        const FunctionDecl* Callee = Call->getDirectCallee();
        if (!Callee) return;
        const bool CBoundary = Callee->isExternC();
        const bool Registration = isExitOrTerminateRegistration(Callee);
        if (!CBoundary && !Registration) return;

        for (const Expr* Arg : Call->arguments()) {
            const FunctionDecl* Callback = callableFunction(Arg);
            if (!Callback || isEffectivelyNoexcept(Callback)) continue;
            diag(Arg->getExprLoc(),
                 "function '%0' passed to %1 shall be noexcept")
                << Callback->getQualifiedNameAsString()
                << (Registration ? "an exit or terminate handler"
                                 : "an extern C function");
        }
        return;
    }

    // Sub-clause 1: static/thread variable initializer.  This includes block
    // scope statics, calls returning scalar values, and construction inside a
    // new-expression used by a static pointer initializer.
    if (const auto* VD = Result.Nodes.getNodeAs<VarDecl>("static_var")) {
        const FunctionDecl* Throwing = nullptr;
        if (const Expr* Init = VD->getInit()) {
            ThrowingInitializerVisitor V;
            V.TraverseStmt(const_cast<Expr*>(Init));
            Throwing = V.Throwing;
        } else {
            // Default-initialization: iterate ctors to find the default one.
            if (const auto* RD = VD->getType()->getAsCXXRecordDecl()) {
                for (const CXXConstructorDecl* C : RD->ctors()) {
                    if (C->isDefaultConstructor() && !C->isImplicit()) {
                        Throwing = C;
                        break;
                    }
                }
                // If no user-provided default ctor, implicit is noexcept by default.
            }
        }

        if (Throwing && !isEffectivelyNoexcept(Throwing)) {
            diag(VD->getLocation(),
                 "function '%0' used to initialize variable '%1' with static "
                 "or thread storage duration shall be noexcept")
                << Throwing->getQualifiedNameAsString() << VD->getName();
        }
        return;
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
