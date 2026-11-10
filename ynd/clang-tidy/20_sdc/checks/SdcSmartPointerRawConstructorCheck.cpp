#include "SdcSmartPointerRawConstructorCheck.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
void SdcSmartPointerRawConstructorCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(cxxConstructExpr(
        hasDeclaration(cxxConstructorDecl(ofClass(cxxRecordDecl(
            hasAnyName("::std::unique_ptr", "::std::shared_ptr"))))),
        unless(isExpansionInSystemHeader())).bind("construct"), this);
}
void SdcSmartPointerRawConstructorCheck::check(const MatchFinder::MatchResult& Result) {
    const auto* E = Result.Nodes.getNodeAs<CXXConstructExpr>("construct");
    const auto* Ctor = E->getConstructor();
    if (Ctor->getNumParams() == 0 || E->getNumArgs() == 0) return;
    // The ownership-taking overloads take the pointer first. Copy/move and
    // shared_ptr's aliasing constructor take another smart pointer first.
    if (!Ctor->getParamDecl(0)->getType()->isPointerType()) return;
    emitPolicyDiagnostic(*this, DynTypedNode::create(*E), E->getExprLoc(),
        "do not use ownership-taking raw pointer constructors of smart pointers; use make_unique or make_shared",
        *Result.Context, Instances);
}
}
