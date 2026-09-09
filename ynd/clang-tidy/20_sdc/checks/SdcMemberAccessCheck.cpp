#include "SdcMemberAccessCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
void SdcMemberAccessCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcMemberAccessCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto *R = dyn_cast<CXXRecordDecl>(D);
    SourceLocation L = D->getLocation();
    StringRef Message;
    if (R && R->isThisDeclarationADefinition() && !R->isLambda()) {
        bool Public = false, Private = false, Protected = false;
        for (const auto *Member : R->fields()) {
            if (Member->isUnnamedBitField()) continue;
            Public |= Member->getAccess() == AS_public;
            Private |= Member->getAccess() == AS_private;
            Protected |= Member->getAccess() == AS_protected;
        }
        if (Protected || (Public && Private))
            Message = "non-static data members should be either all private or all public";
    }
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
