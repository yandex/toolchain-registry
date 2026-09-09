#include "SdcNoVectorBoolCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
void SdcNoVectorBoolCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(typeLoc().bind("type"), this);
}

void SdcNoVectorBoolCheck::check(const MatchFinder::MatchResult &Result) {
    const auto *TL = Result.Nodes.getNodeAs<TypeLoc>("type");
    if (!TL) return;
    auto &C = *Result.Context;
    SourceLocation L = TL->getBeginLoc();
    bool Bad = false;
    auto T = TL->getAs<TemplateSpecializationTypeLoc>();
    if (!T) return;
    const auto *D = T.getTypePtr()->getTemplateName().getAsTemplateDecl();
    Bad = D && D->getName() == "vector" && D->isInStdNamespace() &&
          T.getNumArgs() && T.getArgLoc(0).getArgument().getKind() == TemplateArgument::Type &&
          T.getArgLoc(0).getArgument().getAsType()->isBooleanType();
    if (Bad && isInAnalyzedCode(*TL, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*TL), L, "std::vector should not be specialized with bool", C, Instances);
}
} // namespace clang::tidy::sdc
