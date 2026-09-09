#include "SdcSymmetricalOperatorsCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
void SdcSymmetricalOperatorsCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(decl(unless(isImplicit())).bind("decl"), this);
}

void SdcSymmetricalOperatorsCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
    if (!D || isa<TranslationUnitDecl>(D)) return;
    const auto *M = dyn_cast<CXXMethodDecl>(D);
    SourceLocation L = D->getLocation();
    StringRef Message;
    if (M && !M->isStatic() && M->getNumParams() == 1) {
        switch (M->getOverloadedOperator()) {
        case OO_Plus: case OO_Minus: case OO_Star: case OO_Slash: case OO_Percent:
        case OO_EqualEqual: case OO_ExclaimEqual: case OO_Less: case OO_LessEqual:
        case OO_Greater: case OO_GreaterEqual: case OO_Caret: case OO_Amp: case OO_Pipe:
        case OO_AmpAmp: case OO_PipePipe:
            Message = "implement symmetrical binary operators as non-member functions"; break;
        default: break;
        }
    }
    L = D->getBeginLoc();
    if (!Message.empty() && isInAnalyzedCode(*D, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*D), L, Message, C, Instances);
}
} // namespace clang::tidy::sdc
