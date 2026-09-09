#include "SdcInternalLinkageCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "SdcDeclarationUtils.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
using namespace declaration_detail;
namespace {
bool anonymousScope(const DeclContext *DC) {
    for (; DC; DC = DC->getParent())
        if (const auto *NS = dyn_cast<NamespaceDecl>(DC))
            if (NS->isAnonymousNamespace()) return true;
    return false;
}
} // namespace
void SdcInternalLinkageCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcInternalLinkageCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto *V = dyn_cast<VarDecl>(D);
    const auto *F = dyn_cast<FunctionDecl>(D);
    SourceLocation L = D->getLocation();
    StringRef Message;
    if (V && (V->isConstexpr() || V->getType().isConstQualified())) return;
    if ((V || F) && namespaceScope(D->getDeclContext())) {
        const auto *N = cast<NamedDecl>(D);
        StorageClass SC = V ? V->getStorageClass() : F->getStorageClass();
        if (N->getFormalLinkage() == Linkage::Internal &&
            (!anonymousScope(D->getDeclContext()) || SC == SC_Static || SC == SC_Extern))
            Message = "specify internal linkage using an unnamed namespace without static or extern";
    }
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
