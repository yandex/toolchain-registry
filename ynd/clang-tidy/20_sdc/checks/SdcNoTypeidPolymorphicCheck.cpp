#include "SdcNoTypeidPolymorphicCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

SdcNoTypeidPolymorphicCheck::SdcNoTypeidPolymorphicCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcNoTypeidPolymorphicCheck::registerMatchers(MatchFinder* Finder) {
    // There is no dedicated cxxTypeidExpr() matcher in clang-20.
    // Match all function definitions and walk them manually.
    Finder->addMatcher(
        functionDecl(isDefinition(), unless(isExpansionInSystemHeader())).bind("fn"),
        this);
    // Also match variable declarations with initialisers (e.g. global typeid use).
    Finder->addMatcher(
        varDecl(isDefinition(), unless(isExpansionInSystemHeader()),
                has(expr())).bind("var"),
        this);
}

namespace {

class TypeidVisitor : public RecursiveASTVisitor<TypeidVisitor> {
public:
    TypeidVisitor(SdcNoTypeidPolymorphicCheck* Check, ASTContext* Ctx)
        : Check(Check), Ctx(Ctx) {}

    bool VisitCXXTypeidExpr(CXXTypeidExpr* TE) {
        // typeid(type-id) form — rule does not apply.
        if (TE->isTypeOperand()) return true;

        if (TE->getBeginLoc().isInvalid()) return true;
        if (Ctx->getSourceManager().isInSystemHeader(TE->getBeginLoc()))
            return true;

        // Get the static type of the expression operand.
        QualType T = TE->getExprOperand()
                        ->getType()
                        .getNonReferenceType()
                        .getCanonicalType();

        const auto* RD = T->getAsCXXRecordDecl();
        if (!RD) return true; // not a class type (void, int, dependent, …)

        if (!RD->isPolymorphic()) return true;

        Check->diag(TE->getBeginLoc(),
                    "operand to typeid is of polymorphic class type '%0'; "
                    "the result depends on the dynamic type and requires RTTI")
            << T.getUnqualifiedType();
        return true;
    }

private:
    SdcNoTypeidPolymorphicCheck* Check;
    ASTContext* Ctx;
};

} // namespace

void SdcNoTypeidPolymorphicCheck::check(
    const MatchFinder::MatchResult& Result) {

    ASTContext* Ctx = Result.Context;
    TypeidVisitor V(this, Ctx);

    if (const auto* FD = Result.Nodes.getNodeAs<FunctionDecl>("fn")) {
        if (FD->hasBody())
            V.TraverseStmt(FD->getBody());
        return;
    }

    if (const auto* VD = Result.Nodes.getNodeAs<VarDecl>("var")) {
        if (const Expr* Init = VD->getInit())
            V.TraverseStmt(const_cast<Expr*>(Init));
        return;
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
