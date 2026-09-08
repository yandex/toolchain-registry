#include "SdcNoSetlocaleGlobalCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcNoSetlocaleGlobalCheck::SdcNoSetlocaleGlobalCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            namespace {

            bool isCharPointer(QualType Type, bool RequireConstPointee) {
                const auto* Pointer = Type->getAs<PointerType>();
                if (!Pointer)
                    return false;
                const QualType Pointee = Pointer->getPointeeType();
                return Pointee->isCharType() &&
                       (!RequireConstPointee || Pointee.isConstQualified());
            }

            bool isStandardSetlocale(const FunctionDecl& Function,
                                     const ASTContext& Context) {
                const std::string Name = Function.getQualifiedNameAsString();
                return (Name == "setlocale" || Name == "std::setlocale") &&
                       Function.getNumParams() == 2 &&
                       isCharPointer(Function.getReturnType(), false) &&
                       Context.hasSameType(
                           Function.getParamDecl(0)->getType()
                               .getUnqualifiedType(),
                           Context.IntTy) &&
                       isCharPointer(Function.getParamDecl(1)->getType(), true);
            }

            bool isStandardLocaleGlobal(const FunctionDecl& Function) {
                return Function.getQualifiedNameAsString() ==
                       "std::locale::global";
            }

            } // namespace

            void SdcNoSetlocaleGlobalCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    callExpr(
                        callee(functionDecl(hasName("setlocale"))),
                        unless(isExpansionInSystemHeader()))
                        .bind("setlocale_call"),
                    this);

                Finder->addMatcher(
                    callExpr(
                        callee(functionDecl(hasName("std::locale::global"))),
                        unless(isExpansionInSystemHeader()))
                        .bind("locale_global_call"),
                    this);

                // Also match setlocale when called through using namespace std using unresolvedLookupExpr
                Finder->addMatcher(
                    callExpr(
                        callee(unresolvedLookupExpr(hasAnyDeclaration(namedDecl(hasName("setlocale"))))),
                        unless(isExpansionInSystemHeader()))
                        .bind("unresolved_setlocale_call"),
                    this);
            }

            void SdcNoSetlocaleGlobalCheck::check(
                const MatchFinder::MatchResult& Result) {
                if (const auto* SetlocaleCall = Result.Nodes.getNodeAs<clang::CallExpr>("setlocale_call")) {
                    const FunctionDecl* Function =
                        SetlocaleCall->getDirectCallee();
                    if (!Function ||
                        !isStandardSetlocale(*Function, *Result.Context)) {
                        return;
                    }
                    for (const Decl* Instance : AnalysisInstances.claim(
                             *SetlocaleCall, SetlocaleCall->getBeginLoc(),
                             *Result.Context)) {
                        (void)Instance;
                        diag(SetlocaleCall->getBeginLoc(),
                             "the setlocale function shall not be called");
                    }
                    return;
                }

                // Check for unresolved setlocale calls (like when using namespace std)
                if (const auto* UnresolvedCall = Result.Nodes.getNodeAs<clang::CallExpr>("unresolved_setlocale_call")) {
                    const auto* Lookup = dyn_cast<UnresolvedLookupExpr>(
                        UnresolvedCall->getCallee()->IgnoreParenImpCasts());
                    bool HasStandardSetlocale = false;
                    if (Lookup) {
                        for (const NamedDecl* Candidate : Lookup->decls()) {
                            if (const auto* Function =
                                    dyn_cast<FunctionDecl>(Candidate)) {
                                HasStandardSetlocale |= isStandardSetlocale(
                                    *Function, *Result.Context);
                            }
                        }
                    }
                    if (!HasStandardSetlocale) {
                        return;
                    }
                    for (const Decl* Instance : AnalysisInstances.claim(
                             *UnresolvedCall, UnresolvedCall->getBeginLoc(),
                             *Result.Context)) {
                        (void)Instance;
                        diag(UnresolvedCall->getBeginLoc(),
                             "the setlocale function shall not be called");
                    }
                    return;
                }

                // Check for std::locale::global calls
                if (const auto* LocaleGlobalCall = Result.Nodes.getNodeAs<clang::CallExpr>("locale_global_call")) {
                    const FunctionDecl* Function =
                        LocaleGlobalCall->getDirectCallee();
                    if (!Function || !isStandardLocaleGlobal(*Function)) {
                        return;
                    }
                    for (const Decl* Instance : AnalysisInstances.claim(
                             *LocaleGlobalCall,
                             LocaleGlobalCall->getBeginLoc(),
                             *Result.Context)) {
                        (void)Instance;
                        diag(LocaleGlobalCall->getBeginLoc(),
                             "the std::locale::global function shall not be "
                             "called");
                    }
                    return;
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
