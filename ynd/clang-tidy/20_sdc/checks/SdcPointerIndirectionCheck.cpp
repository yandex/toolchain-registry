#include "SdcPointerIndirectionCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

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
    const auto *F = dyn_cast<FunctionDecl>(D);
    const auto *Field = dyn_cast<FieldDecl>(D);
    SourceLocation L = D->getLocation();
    StringRef Message;
    if ((V && tooManyPointers(V->getType())) || (Field && tooManyPointers(Field->getType())))
        Message = "an object declaration should contain no more than two levels of pointer indirection";
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
