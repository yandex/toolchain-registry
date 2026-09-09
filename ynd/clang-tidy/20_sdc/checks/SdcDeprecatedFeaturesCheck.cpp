#include "SdcDeprecatedFeaturesCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/StringSwitch.h"

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
namespace {
bool oneOf(StringRef N, std::initializer_list<StringRef> Names) {
    for (auto Candidate : Names) if (Candidate == N) return true;
    return false;
}
bool standard(const NamedDecl *D) {
    return D && D->isInStdNamespace();
}
bool deprecatedName(const NamedDecl *D, const CXXRecordDecl *WrittenOwner = nullptr) {
    if (!D || !D->getIdentifier()) return false;
    StringRef N = D->getName();
    if (standard(D) && oneOf(N, {"strstreambuf", "istrstream", "ostrstream", "strstream",
        "uncaught_exception", "unary_negate", "binary_negate", "not1", "not2", "raw_storage_iterator",
        "get_temporary_buffer", "return_temporary_buffer", "is_literal_type", "is_literal_type_v",
        "result_of", "result_of_t", "iterator", "codecvt_mode", "consume_header", "generate_header",
        "little_endian", "codecvt_utf8", "codecvt_utf16", "codecvt_utf8_utf16", "wstring_convert", "wbuffer_convert"}))
        return true;
    const auto *Parent = WrittenOwner ? WrittenOwner : dyn_cast<CXXRecordDecl>(D->getDeclContext());
    if (!Parent) return false;
    const auto *Owner = Parent;
    while (const auto *Outer = dyn_cast<CXXRecordDecl>(Owner->getDeclContext())) Owner = Outer;
    if (!standard(Owner)) return false;
    StringRef P = Owner->getName();
    if (P == "shared_ptr" && N == "unique") return true;
    if (P == "allocator") {
        if (oneOf(N, {"size_type", "difference_type", "pointer", "const_pointer", "reference",
                      "const_reference", "rebind", "address", "construct", "destroy", "max_size"})) return true;
        if (N == "allocate")
            if (const auto *F = dyn_cast<FunctionDecl>(D)) return F->getNumParams() == 2;
    }
    if (oneOf(N, {"argument_type", "first_argument_type", "second_argument_type", "result_type"})) {
        if (P == "function") return N != "result_type";
        return oneOf(P, {"owner_less", "reference_wrapper", "plus", "minus", "multiplies", "divides",
                        "modulus", "negate", "equal_to", "not_equal_to", "greater", "less", "greater_equal",
                        "less_equal", "logical_and", "logical_or", "logical_not", "bit_and", "bit_or",
                        "bit_xor", "bit_not", "hash"}) ||
               ((P == "map" || P == "multimap") && Parent->getName() == "value_compare") ||
               P.starts_with("_Bind") || P.starts_with("_Mem_fn") || P.starts_with("_Weak_result_type");
    }
    return false;
}
class DeprecatedHeaders : public PPCallbacks {
public:
    DeprecatedHeaders(SdcDeprecatedFeaturesCheck &Check, const SourceManager &SM) : Check(Check), SM(SM) {}
    void InclusionDirective(SourceLocation HashLoc, const Token &, StringRef FileName, bool,
                            CharSourceRange, OptionalFileEntryRef, StringRef, StringRef,
                            const Module *, bool, SrcMgr::CharacteristicKind FileType) override {
        if (FileType != SrcMgr::C_User && isWrittenInAnalyzedSource(HashLoc, SM) &&
            oneOf(FileName, {"codecvt", "strstream", "ccomplex", "cstdalign", "cstdbool", "ctgmath"}))
            Check.diag(HashLoc, "do not include a C++17-deprecated standard library header");
    }
private:
    SdcDeprecatedFeaturesCheck &Check;
    const SourceManager &SM;
};
bool deprecatedCopy(const CXXMethodDecl *M) {
    if (!M || !M->isImplicit()) return false;
    const auto *R = M->getParent();
    if (const auto *Ctor = dyn_cast<CXXConstructorDecl>(M))
        return Ctor->isCopyConstructor() && (R->hasUserDeclaredCopyAssignment() || R->hasUserDeclaredDestructor());
    return M->isCopyAssignmentOperator() && (R->hasUserDeclaredCopyConstructor() || R->hasUserDeclaredDestructor());
}
} // namespace
void SdcDeprecatedFeaturesCheck::registerPPCallbacks(const SourceManager &SM, Preprocessor *PP, Preprocessor *) {
    PP->addPPCallbacks(std::make_unique<DeprecatedHeaders>(*this, SM));
}
void SdcDeprecatedFeaturesCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(varDecl(unless(isImplicit())).bind("variable"), this);
    Finder->addMatcher(expr().bind("expr"), this);
    Finder->addMatcher(typeLoc().bind("type"), this);
}
void SdcDeprecatedFeaturesCheck::check(const MatchFinder::MatchResult &Result) {
    auto &C = *Result.Context;
    DynTypedNode Node;
    SourceLocation L;
    bool Bad = false;
    if (const auto *V = Result.Nodes.getNodeAs<VarDecl>("variable")) {
        Node = DynTypedNode::create(*V); L = V->getLocation();
        Bad = V->isStaticDataMember() && V->isConstexpr() && V->isOutOfLine() && !V->hasInit();
    } else if (const auto *E = Result.Nodes.getNodeAs<Expr>("expr")) {
        Node = DynTypedNode::create(*E); L = E->getExprLoc();
        if (const auto *Ref = dyn_cast<DeclRefExpr>(E)) Bad = deprecatedName(Ref->getDecl());
        if (const auto *Member = dyn_cast<MemberExpr>(E)) {
            auto T = Member->getBase()->IgnoreParenImpCasts()->getType();
            if (T->isPointerType()) T = T->getPointeeType();
            Bad = deprecatedName(Member->getMemberDecl(), T->getAsCXXRecordDecl());
            if (Member->getMemberDecl()->getName() == "allocate")
                for (const auto &P : C.getParents(*Member))
                    if (const auto *Call = P.get<CXXMemberCallExpr>())
                        if (Call->getNumArgs() == 2 && isa<CXXDefaultArgExpr>(Call->getArg(1))) Bad = false;
        }
        if (const auto *Construct = dyn_cast<CXXConstructExpr>(E)) Bad = deprecatedCopy(Construct->getConstructor());
        if (const auto *Call = dyn_cast<CXXOperatorCallExpr>(E))
            Bad = deprecatedCopy(dyn_cast_or_null<CXXMethodDecl>(Call->getDirectCallee()));
    } else if (const auto *TL = Result.Nodes.getNodeAs<TypeLoc>("type")) {
        Node = DynTypedNode::create(*TL); L = TL->getBeginLoc();
        if (auto T = TL->getAs<FunctionProtoTypeLoc>()) {
            Bad = T.getTypePtr()->getExceptionSpecType() == EST_DynamicNone;
            if (Bad) L = T.getExceptionSpecRange().getBegin();
        }
        if (auto T = TL->getAs<ElaboratedTypeLoc>()) {
            const auto *Q = T.getQualifierLoc().getNestedNameSpecifier();
            auto Name = T.getNamedTypeLoc().getAs<TypedefTypeLoc>();
            if (Q && Q->getAsType() && Name) {
                Bad = deprecatedName(Name.getTypedefNameDecl(), Q->getAsType()->getAsCXXRecordDecl());
                L = Name.getBeginLoc();
            }
        }
        if (auto T = TL->getAs<TypedefTypeLoc>()) Bad = deprecatedName(T.getTypedefNameDecl());
        if (auto T = TL->getAs<TagTypeLoc>()) Bad = deprecatedName(T.getDecl());
        if (auto T = TL->getAs<TemplateSpecializationTypeLoc>()) {
            const auto *D = T.getTypePtr()->getTemplateName().getAsTemplateDecl();
            Bad = deprecatedName(D);
            if (standard(D) && D->getName() == "allocator" && T.getNumArgs() &&
                T.getArgLoc(0).getArgument().getKind() == TemplateArgument::Type)
                Bad |= T.getArgLoc(0).getArgument().getAsType()->isVoidType();
        }
    }
    if (Bad && isInAnalyzedCode(Node, L, C))
        emitPolicyDiagnostic(*this, Node, L, "do not use a feature deprecated by C++17 Annex D", C, Instances);
}
} // namespace clang::tidy::sdc
