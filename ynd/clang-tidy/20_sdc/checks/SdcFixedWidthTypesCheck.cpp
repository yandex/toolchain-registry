#include "SdcFixedWidthTypesCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
void SdcFixedWidthTypesCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(typeLoc().bind("type"), this);
}

void SdcFixedWidthTypesCheck::check(const MatchFinder::MatchResult &Result) {
    const auto *TL = Result.Nodes.getNodeAs<TypeLoc>("type");
    if (!TL) return;
    auto &C = *Result.Context;
    SourceLocation L = TL->getBeginLoc();
    bool Bad = false;
    auto B = TL->getAs<BuiltinTypeLoc>();
    if (!B) return;
    const auto K = B.getTypePtr()->getKind();
    Bad = K == BuiltinType::SChar || K == BuiltinType::UChar ||
          K == BuiltinType::Short || K == BuiltinType::UShort ||
          K == BuiltinType::Int || K == BuiltinType::UInt ||
          K == BuiltinType::Long || K == BuiltinType::ULong ||
          K == BuiltinType::LongLong || K == BuiltinType::ULongLong;
    DynTypedNode N = DynTypedNode::create(*TL);
    for (unsigned I = 0; Bad && I != 32; ++I) {
        auto Parents = C.getParents(N);
        if (Parents.empty()) break;
        N = Parents[0];
        if (N.get<TypedefNameDecl>()) { Bad = false; break; }
        if (const auto *P = N.get<ParmVarDecl>()) {
            const auto *F = dyn_cast<FunctionDecl>(P->getDeclContext());
            if (F && K == BuiltinType::Int && P->getType()->isSpecificBuiltinType(BuiltinType::Int) &&
                (F->isMain() || ((F->getOverloadedOperator() == OO_PlusPlus ||
                                F->getOverloadedOperator() == OO_MinusMinus) &&
                               P == F->getParamDecl(F->getNumParams() - 1)))) Bad = false;
            break;
        }
        if (const auto *F = N.get<FunctionDecl>()) {
            if (F->isMain() && K == BuiltinType::Int) Bad = false;
            break;
        }
        if (N.get<Decl>()) break;
    }
    if (Bad && isInAnalyzedCode(*TL, L, C))
        emitPolicyDiagnostic(*this, DynTypedNode::create(*TL), L, "use a named fixed-width integer type instead of a standard integer type name", C, Instances);
}
} // namespace clang::tidy::sdc
