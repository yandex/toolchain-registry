#include "SdcNoArrayVariablesCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
void SdcNoArrayVariablesCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcNoArrayVariablesCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto *V = dyn_cast<VarDecl>(D);
    const auto *Field = dyn_cast<FieldDecl>(D);
    SourceLocation L = D->getLocation();
    StringRef Message;
    if (V || Field) {
        QualType T = V ? V->getType() : Field->getType();
        const Expr *Init = V ? V->getInit() : Field->getInClassInitializer();
        if (Init) {
            if (const auto *List = dyn_cast<InitListExpr>(Init->IgnoreParenImpCasts()))
                if (List->getNumInits() == 1) Init = List->getInit(0);
        }
        if (const auto *A = C.getAsArrayType(T)) {
            QualType E = A->getElementType();
            if (!(E.isConstQualified() && E->isAnyCharacterType() && Init &&
                  isa<StringLiteral>(Init->IgnoreParenImpCasts())))
                Message = "use a container instead of declaring a variable of array type";
        }
    }
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
