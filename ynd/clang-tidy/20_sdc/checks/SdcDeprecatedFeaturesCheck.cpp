#include "SdcDeprecatedFeaturesCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include <string>

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
unsigned standardYear(const LangOptions &LO) {
    if (LO.CPlusPlus26) return 2026;
    if (LO.CPlusPlus23) return 2023;
    if (LO.CPlusPlus20) return 2020;
    if (LO.CPlusPlus17) return 2017;
    if (LO.CPlusPlus14) return 2014;
    if (LO.CPlusPlus11) return 2011;
    return 1998;
}
std::string standardName(unsigned Year) {
    return "C++" + std::to_string(Year % 100);
}
struct Deprecation {
    unsigned Since = 0;
    unsigned Removed = 0;
    std::string Feature;
    unsigned Until = 0;

    std::string message(unsigned Active) const {
        if (!Since || Active < Since || (Until && Active >= Until)) return {};
        if (Removed && Active >= Removed)
            return Feature + " was removed in " + standardName(Removed) +
                   " (compiling as " + standardName(Active) + ")";
        return Feature + " is deprecated in " + standardName(Active) +
               " (since " + standardName(Since) + ")";
    }
};
bool programDefinedSpecializationMember(const NamedDecl *D, const SourceManager &SM) {
    // A program's std::hash<T> may define its own result_type, for example.
    // Inspect the declaring class, not the qualifier at the use: inherited
    // library aliases must still be recognized as library declarations.
    for (const DeclContext *DC = D->getDeclContext();
         const auto *R = dyn_cast<CXXRecordDecl>(DC); DC = R->getDeclContext()) {
        const auto *Pattern = R->getTemplateInstantiationPattern();
        if (!Pattern) Pattern = R;
        const auto *Specialization = dyn_cast<ClassTemplateSpecializationDecl>(Pattern);
        if (Specialization &&
            (isa<ClassTemplatePartialSpecializationDecl>(Specialization) ||
             Specialization->getSpecializationKind() == TSK_ExplicitSpecialization) &&
            !SM.isInSystemHeader(SM.getSpellingLoc(Pattern->getLocation()))) return true;
    }
    return false;
}
Deprecation deprecatedName(const NamedDecl *D, const SourceManager &SM,
                          const CXXRecordDecl *WrittenOwner = nullptr) {
    if (!D || !D->getIdentifier() || programDefinedSpecializationMember(D, SM)) return {};
    StringRef N = D->getName();
    auto Named = [&](unsigned Since, unsigned Removed = 0) {
        std::string Name = WrittenOwner ? WrittenOwner->getQualifiedNameAsString() + "::" + N.str()
                                       : D->getQualifiedNameAsString();
        return Deprecation{Since, Removed, "standard library entity '" + Name + "'"};
    };
    if (standard(D)) {
        if (oneOf(N, {"strstreambuf", "istrstream", "ostrstream", "strstream"})) return Named(1998, 2026);
        if (oneOf(N, {"auto_ptr", "unary_function", "binary_function", "binder1st", "binder2nd",
                      "bind1st", "bind2nd", "ptr_fun", "pointer_to_unary_function", "pointer_to_binary_function",
                      "mem_fun", "mem_fun_ref", "mem_fun_t", "mem_fun1_t", "const_mem_fun_t",
                      "const_mem_fun1_t", "mem_fun_ref_t", "mem_fun1_ref_t", "const_mem_fun_ref_t",
                      "const_mem_fun1_ref_t"})) return Named(2011, 2017);
        if (oneOf(N, {"uncaught_exception", "unary_negate", "binary_negate", "not1", "not2",
                      "raw_storage_iterator", "get_temporary_buffer", "return_temporary_buffer",
                      "is_literal_type", "is_literal_type_v", "result_of", "result_of_t"})) return Named(2017, 2020);
        if (N == "iterator") return Named(2017);
        if (oneOf(N, {"codecvt_mode", "consume_header", "generate_header", "little_endian",
                      "codecvt_utf8", "codecvt_utf16", "codecvt_utf8_utf16", "wstring_convert",
                      "wbuffer_convert"})) return Named(2017, 2026);
        if (oneOf(N, {"is_pod", "is_pod_v", "atomic_init"})) return Named(2020);
        if (oneOf(N, {"aligned_storage", "aligned_storage_t", "aligned_union", "aligned_union_t"}))
            return Named(2023);
    }
    const auto *Parent = WrittenOwner ? WrittenOwner : dyn_cast<CXXRecordDecl>(D->getDeclContext());
    if (!Parent) return {};
    const auto *Owner = Parent;
    while (const auto *Outer = dyn_cast<CXXRecordDecl>(Owner->getDeclContext())) Owner = Outer;
    if (!standard(Owner)) return {};
    StringRef P = Owner->getName();
    if (P == "shared_ptr" && N == "unique") return Named(2017, 2020);
    if (P == "allocator") {
        // These typedefs were deprecated in C++17 and restored in C++20.
        if (oneOf(N, {"size_type", "difference_type"})) {
            auto D = Named(2017);
            D.Until = 2020;
            return D;
        }
        if (oneOf(N, {"pointer", "const_pointer", "reference", "const_reference", "rebind",
                      "address", "construct", "destroy", "max_size"})) return Named(2017, 2020);
        if (N == "allocate")
            if (const auto *F = dyn_cast<FunctionDecl>(D))
                if (F->getNumParams() == 2) {
                    auto D = Named(2017, 2020);
                    D.Feature = "two-argument overload of " + D.Feature;
                    return D;
                }
    }
    if (oneOf(N, {"argument_type", "first_argument_type", "second_argument_type", "result_type"})) {
        if (P == "function") return N != "result_type" ? Named(2017, 2020) : Deprecation{};
        if (oneOf(P, {"owner_less", "reference_wrapper", "plus", "minus", "multiplies", "divides",
                      "modulus", "negate", "equal_to", "not_equal_to", "greater", "less", "greater_equal",
                      "less_equal", "logical_and", "logical_or", "logical_not", "bit_and", "bit_or",
                      "bit_xor", "bit_not", "hash"}) ||
            ((P == "map" || P == "multimap") && Parent->getName() == "value_compare") ||
            P.starts_with("_Bind") || P.starts_with("_Mem_fn") || P.starts_with("_Weak_result_type"))
            return Named(2017, 2020);
    }
    return {};
}
class DeprecatedHeaders : public PPCallbacks {
public:
    DeprecatedHeaders(SdcDeprecatedFeaturesCheck &Check, const SourceManager &SM, unsigned Active)
        : Check(Check), SM(SM), Active(Active) {}
    void InclusionDirective(SourceLocation HashLoc, const Token &, StringRef FileName, bool,
                            CharSourceRange, OptionalFileEntryRef, StringRef, StringRef,
                            const Module *, bool, SrcMgr::CharacteristicKind FileType) override {
        if (FileType == SrcMgr::C_User || !isWrittenInAnalyzedSource(HashLoc, SM)) return;
        Deprecation D;
        if (FileName == "strstream") D = {1998, 2026, "standard library header <strstream>"};
        else if (FileName == "codecvt") D = {2017, 2026, "standard library header <codecvt>"};
        else if (oneOf(FileName, {"ccomplex", "cstdalign", "cstdbool", "ctgmath"}))
            D = {2017, 2020, "standard library header <" + FileName.str() + ">"};
        auto Message = D.message(Active);
        if (!Message.empty()) Check.diag(HashLoc, "%0") << Message;
    }
private:
    SdcDeprecatedFeaturesCheck &Check;
    const SourceManager &SM;
    unsigned Active;
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
    PP->addPPCallbacks(std::make_unique<DeprecatedHeaders>(*this, SM, standardYear(getLangOpts())));
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
    Deprecation Finding;
    const unsigned Active = standardYear(C.getLangOpts());
    if (const auto *V = Result.Nodes.getNodeAs<VarDecl>("variable")) {
        Node = DynTypedNode::create(*V); L = V->getLocation();
        if (V->isStaticDataMember() && V->isConstexpr() && V->isOutOfLine() && !V->hasInit())
            Finding = {2017, 0, "redundant out-of-class redeclaration of static constexpr member '" +
                       V->getQualifiedNameAsString() + "'"};
    } else if (const auto *E = Result.Nodes.getNodeAs<Expr>("expr")) {
        Node = DynTypedNode::create(*E); L = E->getExprLoc();
        if (const auto *Ref = dyn_cast<DeclRefExpr>(E)) Finding = deprecatedName(Ref->getDecl(), C.getSourceManager());
        if (const auto *Member = dyn_cast<MemberExpr>(E)) {
            auto T = Member->getBase()->IgnoreParenImpCasts()->getType();
            if (T->isPointerType()) T = T->getPointeeType();
            Finding = deprecatedName(Member->getMemberDecl(), C.getSourceManager(), T->getAsCXXRecordDecl());
            if (Member->getMemberDecl()->getIdentifier() &&
                Member->getMemberDecl()->getName() == "allocate")
                for (const auto &P : C.getParents(*Member))
                    if (const auto *Call = P.get<CXXMemberCallExpr>())
                        if (Call->getNumArgs() == 2 && isa<CXXDefaultArgExpr>(Call->getArg(1))) Finding = {};
        }
        const CXXMethodDecl *Copy = nullptr;
        if (const auto *Construct = dyn_cast<CXXConstructExpr>(E)) Copy = Construct->getConstructor();
        if (const auto *Call = dyn_cast<CXXOperatorCallExpr>(E))
            Copy = dyn_cast_or_null<CXXMethodDecl>(Call->getDirectCallee());
        if (deprecatedCopy(Copy)) {
            const auto *R = Copy->getParent();
            Finding = {2011, 0, std::string("implicit copy ") +
                (isa<CXXConstructorDecl>(Copy) ? "constructor" : "assignment operator") +
                " of '" + R->getQualifiedNameAsString() + "' with a user-declared " +
                (R->hasUserDeclaredDestructor() ? "destructor" : "copy operation")};
        }
        if (const auto *Lambda = dyn_cast<LambdaExpr>(E))
            if (Lambda->getCaptureDefault() == LCD_ByCopy)
                for (const auto &Capture : Lambda->captures())
                    if (Capture.capturesThis() && Capture.isImplicit()) {
                        Finding = {2020, 0, "implicit capture of 'this' by a lambda with '[=]'"};
                        L = Lambda->getBeginLoc();
                    }
        if (const auto *B = dyn_cast<BinaryOperator>(E)) {
            if (B->isComparisonOp() &&
                B->getLHS()->IgnoreParenImpCasts()->getType()->isArrayType() &&
                B->getRHS()->IgnoreParenImpCasts()->getType()->isArrayType())
                Finding = {2020, 2026, "comparison between two arrays"};
        }
    } else if (const auto *TL = Result.Nodes.getNodeAs<TypeLoc>("type")) {
        Node = DynTypedNode::create(*TL); L = TL->getBeginLoc();
        if (auto T = TL->getAs<FunctionProtoTypeLoc>()) {
            auto Spec = T.getTypePtr()->getExceptionSpecType();
            if (Spec == EST_DynamicNone || Spec == EST_Dynamic) {
                Finding = {2011, Spec == EST_DynamicNone ? 2020U : 2017U,
                           Spec == EST_DynamicNone ? "exception specification 'throw()'" :
                                                    "dynamic exception specification 'throw(types)'"};
                L = T.getExceptionSpecRange().getBegin();
            }
        }
        if (auto T = TL->getAs<ElaboratedTypeLoc>()) {
            const auto *Q = T.getQualifierLoc().getNestedNameSpecifier();
            auto Name = T.getNamedTypeLoc().getAs<TypedefTypeLoc>();
            if (Q && Q->getAsType() && Name) {
                Finding = deprecatedName(Name.getTypedefNameDecl(), C.getSourceManager(), Q->getAsType()->getAsCXXRecordDecl());
                L = Name.getBeginLoc();
            }
        }
        if (auto T = TL->getAs<TypedefTypeLoc>()) Finding = deprecatedName(T.getTypedefNameDecl(), C.getSourceManager());
        if (auto T = TL->getAs<TagTypeLoc>()) Finding = deprecatedName(T.getDecl(), C.getSourceManager());
        if (auto T = TL->getAs<TemplateSpecializationTypeLoc>()) {
            const auto *D = T.getTypePtr()->getTemplateName().getAsTemplateDecl();
            Finding = deprecatedName(D, C.getSourceManager());
            if (standard(D) && D->getName() == "allocator" && T.getNumArgs() &&
                T.getArgLoc(0).getArgument().getKind() == TemplateArgument::Type)
                if (T.getArgLoc(0).getArgument().getAsType()->isVoidType())
                    Finding = {2017, 2020, "standard library specialization 'std::allocator<void>'"};
        }
    }
    auto Message = Finding.message(Active);
    if (!Message.empty() && isInAnalyzedCode(Node, L, C))
        for (const Decl *Instance : Instances.claim(Node, L, C))
            diagnoseAnalysisInstance(*this, Instance, C, L, "%0", Message);
}
} // namespace clang::tidy::sdc
