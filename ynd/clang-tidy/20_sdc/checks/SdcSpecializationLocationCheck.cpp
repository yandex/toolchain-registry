#include "SdcSpecializationLocationCheck.h"
#include "SdcPolicyDiagnostic.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include <set>

using namespace clang::ast_matchers;
namespace clang::tidy::sdc {
namespace {
class AllowedFiles {
public:
    explicit AllowedFiles(const SourceManager &SM) : SM(SM) {}
    bool add(SourceLocation L) {
        if (L.isInvalid()) return false;
        L = getUltimateWrittenLocation(L, SM);
        auto F = SM.getFileEntryRefForID(SM.getFileID(L));
        if (!F) return false;
        Files.insert(&F->getFileEntry()); return true;
    }
    bool contains(SourceLocation L) const {
        if (L.isInvalid()) return false;
        auto F = SM.getFileEntryRefForID(SM.getFileID(getUltimateWrittenLocation(L, SM)));
        return F && Files.count(&F->getFileEntry());
    }
    bool argument(const TemplateArgument &A) {
        if (A.getKind() == TemplateArgument::Integral || A.getKind() == TemplateArgument::NullPtr) return true;
        if (A.getKind() == TemplateArgument::Pack) {
            for (const auto &Item : A.pack_elements()) if (!argument(Item)) return false;
            return true;
        }
        if (A.getKind() != TemplateArgument::Type) return false;
        QualType T = A.getAsType();
        if (T->isDependentType()) return false;
        if (isa<TypedefType>(T.getTypePtr())) return false;
        while (T->isPointerType() || T->isReferenceType()) T = T->getPointeeType();
        // Missing definitions and compound/template arguments are unknown,
        // never evidence that another permitted file cannot exist.
        if (const auto *R = T->getAs<RecordType>()) {
            if (isa<ClassTemplateSpecializationDecl>(R->getDecl())) return false;
            const auto *D = R->getDecl()->getDefinition();
            return D && add(D->getLocation());
        }
        if (const auto *E = T->getAs<EnumType>()) {
            const auto *D = E->getDecl()->getDefinition();
            return D && add(D->getLocation());
        }
        return T->isBuiltinType();
    }
private:
    const SourceManager &SM;
    std::set<const FileEntry *> Files;
};
} // namespace
void SdcSpecializationLocationCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher(classTemplateSpecializationDecl().bind("specialization"), this);
}
void SdcSpecializationLocationCheck::check(const MatchFinder::MatchResult &Result) {
    const auto *S = Result.Nodes.getNodeAs<ClassTemplateSpecializationDecl>("specialization");
    auto &C = *Result.Context;
    // Initial coverage is full class specializations only. Partial and function
    // specializations require additional argument/source provenance modelling.
    if (!S || isa<ClassTemplatePartialSpecializationDecl>(S) || !S->isExplicitSpecialization() ||
        !S->isThisDeclarationADefinition() || S->isInvalidDecl() ||
        !isInAnalyzedCode(*S, S->getLocation(), C)) return;
    const auto *Primary = S->getSpecializedTemplate()->getTemplatedDecl()->getDefinition();
    if (!Primary) return;
    AllowedFiles Files(*Result.SourceManager);
    if (!Files.add(Primary->getLocation())) return;
    for (const auto *D : Primary->redecls())
        if (!Files.add(D->getLocation())) return;
    for (const auto &A : S->getTemplateArgs().asArray()) if (!Files.argument(A)) return;
    if (Files.contains(S->getLocation())) return;
    for (const Decl *Instance : Instances.claim(*S, S->getLocation(), C)) {
        diagnoseAnalysisInstance(*this, Instance, C, S->getLocation(),
            "define this specialization in the primary template's file or a specialized argument's definition file");
        diag(Primary->getLocation(), "primary template is defined here", DiagnosticIDs::Note);
    }
}
} // namespace clang::tidy::sdc
