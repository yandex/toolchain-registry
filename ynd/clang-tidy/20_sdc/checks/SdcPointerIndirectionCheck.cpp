#include "SdcPointerIndirectionCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/STLExtras.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
namespace {
bool tooManyPointers(QualType T, unsigned Depth = 0) {
    if (T.isNull()) return false;
    T = T.getCanonicalType();
    if (T->isPointerType() || T->isMemberPointerType()) {
        if (++Depth > 2) return true;
        return tooManyPointers(T->getPointeeType(), Depth);
    }
    if (T->isReferenceType()) return tooManyPointers(T->getPointeeType(), Depth);
    if (const auto *A = dyn_cast<ArrayType>(T))
        return tooManyPointers(A->getElementType(), Depth);
    if (const auto *F = T->getAs<FunctionProtoType>()) {
        if (tooManyPointers(F->getReturnType())) return true;
        for (auto P : F->param_types()) if (tooManyPointers(P)) return true;
    }
    return false;
}
} // namespace
void SdcPointerIndirectionCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcPointerIndirectionCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto *V = dyn_cast<VarDecl>(D);
    const auto *Field = dyn_cast<FieldDecl>(D);
    if (const auto *Parameter = dyn_cast<ParmVarDecl>(D)) {
        // TypeLoc traversal also visits parameters in aliases and nested
        // function types. They are not objects; inspect the enclosing object's
        // complete type instead, including callback parameters and return types.
        const auto *Owner = dyn_cast<FunctionDecl>(Parameter->getDeclContext());
        if (!Owner || !llvm::is_contained(Owner->parameters(), Parameter)) return;
    }
    QualType Type = V ? V->getType() : Field ? Field->getType() : QualType();
    SourceLocation L = D->getBeginLoc();
    if (!tooManyPointers(Type) || !isInAnalyzedCode(*D, L, C)) return;
    const auto *Named = cast<NamedDecl>(D);
    std::string Name = Named->getQualifiedNameAsString();
    if (Name.empty()) Name = "<unnamed>";
    for (const Decl *Instance : Instances.claim(*D, L, C))
        diagnoseAnalysisInstance(*this, Instance, C, L,
            "object declaration '%0' has type %1 containing more than two levels "
            "of pointer indirection", Name, policyTypeName(Type, C));
}
} // namespace clang::tidy::sdc
