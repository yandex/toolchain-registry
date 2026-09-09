#include "SdcUnusedTypesCheck.h"
#include "SdcCodeSelection.h"
#include "clang/AST/Attr.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include <map>
#include <set>

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
namespace {
const NamedDecl *typeIdentity(const NamedDecl *D) {
    if (const auto *S = dyn_cast<ClassTemplateSpecializationDecl>(D)) D = S->getSpecializedTemplate();
    if (const auto *R = dyn_cast<CXXRecordDecl>(D))
        if (R->getDescribedClassTemplate()) D = R->getDescribedClassTemplate();
    if (const auto *A = dyn_cast<TypeAliasDecl>(D))
        if (A->getDescribedAliasTemplate()) D = A->getDescribedAliasTemplate();
    return cast<NamedDecl>(D->getCanonicalDecl());
}
bool limited(const DeclContext *DC) {
    for (; DC; DC = DC->getParent()) {
        if (DC->isFunctionOrMethod()) return true;
        if (const auto *NS = dyn_cast<NamespaceDecl>(DC)) if (NS->isAnonymousNamespace()) return true;
    }
    return false;
}
bool inDefinition(const Decl *D, const NamedDecl *Identity) {
    if (const auto *N = dyn_cast<NamedDecl>(D)) if (typeIdentity(N) == Identity) return true;
    for (const DeclContext *DC : {D->getDeclContext(), D->getLexicalDeclContext()}) {
        for (; DC; DC = DC->getParent()) {
            if (const auto *N = dyn_cast<NamedDecl>(DC)) if (typeIdentity(N) == Identity) return true;
            // An out-of-class member or hidden friend belongs to the definition.
            if (const auto *F = dyn_cast<FunctionDecl>(DC)) {
                auto *Lex = F->getLexicalDeclContext();
                if (const auto *N = dyn_cast<NamedDecl>(Lex)) if (typeIdentity(N) == Identity) return true;
            }
        }
    }
    return false;
}
class TypeInventory : public RecursiveASTVisitor<TypeInventory> {
public:
    explicit TypeInventory(ASTContext &C) : C(C) {}
    std::map<const NamedDecl *, const NamedDecl *> Candidates;
    std::set<const NamedDecl *> Used;
    bool VisitNamedDecl(NamedDecl *D) {
        if ((!isa<TypeDecl>(D) && !isa<ClassTemplateDecl>(D) && !isa<TypeAliasTemplateDecl>(D)) ||
            isa<TemplateTypeParmDecl>(D) || isa<ClassTemplateSpecializationDecl>(D) || D->isImplicit() ||
            !limited(D->getDeclContext())) return true;
        if (const auto *R = dyn_cast<CXXRecordDecl>(D)) if (R->isLambda() || R->isInjectedClassName()) return true;
        const auto *ID = typeIdentity(D);
        if (D->hasAttr<UnusedAttr>()) Used.insert(ID);
        Candidates.emplace(ID, D);
        return true;
    }
    void use(const NamedDecl *D, const DynTypedNode &N) {
        if (!D) return;
        const auto *ID = typeIdentity(D);
        llvm::SmallVector<DynTypedNode, 8> Work{N};
        llvm::SmallPtrSet<const void *, 16> Seen;
        while (!Work.empty()) {
            auto Current = Work.pop_back_val();
            if (const auto *Decl = Current.get<clang::Decl>()) {
                if (inDefinition(Decl, ID)) return;
                continue;
            }
            for (const auto &P : C.getParents(Current)) {
                const void *Key = P.getMemoizationData();
                if (!Key || Seen.insert(Key).second) Work.push_back(P);
            }
        }
        Used.insert(ID);
    }
    bool VisitTypeLoc(TypeLoc TL) {
        const NamedDecl *D = nullptr;
        if (auto T = TL.getAs<TagTypeLoc>()) D = T.getDecl();
        if (auto T = TL.getAs<TypedefTypeLoc>()) D = T.getTypedefNameDecl();
        if (auto T = TL.getAs<TemplateSpecializationTypeLoc>()) D = T.getTypePtr()->getTemplateName().getAsTemplateDecl();
        use(D, DynTypedNode::create(TL)); return true;
    }
    bool VisitDeclRefExpr(DeclRefExpr *E) {
        if (const auto *EC = dyn_cast<EnumConstantDecl>(E->getDecl()))
            use(cast<EnumDecl>(EC->getDeclContext()), DynTypedNode::create(*E));
        if (const auto *IF = dyn_cast<IndirectFieldDecl>(E->getDecl()))
            for (const auto *D : IF->chain())
                if (const auto *F = dyn_cast<FieldDecl>(D))
                    if (F->getParent()->isAnonymousStructOrUnion()) use(F->getParent(), DynTypedNode::create(*E));
        return true;
    }
    bool VisitMemberExpr(MemberExpr *E) {
        if (const auto *F = dyn_cast<FieldDecl>(E->getMemberDecl()))
            if (F->getParent()->isAnonymousStructOrUnion()) use(F->getParent(), DynTypedNode::create(*E));
        return true;
    }
private:
    ASTContext &C;
};
} // namespace
void SdcUnusedTypesCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(translationUnitDecl().bind("tu"), this);
}
void SdcUnusedTypesCheck::check(const MatchFinder::MatchResult &Result) {
    TypeInventory Inventory(*Result.Context);
    // The unused-type policy explicitly counts references in dormant definitions, including
    // discarded template branches; POL-MAT-001's default does not apply here.
    Inventory.TraverseDecl(Result.Context->getTranslationUnitDecl());
    for (auto [ID, D] : Inventory.Candidates)
        if (!Inventory.Used.count(ID) && isWrittenInAnalyzedSource(D->getLocation(), *Result.SourceManager))
            diag(D->getLocation(), "use this type outside its definition or declare it maybe_unused");
}
} // namespace clang::tidy::sdc
