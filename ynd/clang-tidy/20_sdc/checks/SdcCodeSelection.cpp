#include "SdcCodeSelection.h"

#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

#include <functional>

namespace clang {
namespace tidy {
namespace sdc {

namespace {

TemplateMaterializationKind classifyOneNode(const DynTypedNode& Node) {
    if (const auto* FD = Node.get<FunctionDecl>()) {
        if (::clang::isTemplateInstantiation(
                FD->getTemplateSpecializationKind())) {
            return TemplateMaterializationKind::Instantiation;
        }
        if (FD->getDescribedFunctionTemplate() || FD->isDependentContext()) {
            return TemplateMaterializationKind::UninstantiatedPattern;
        }
    }

    if (const auto* RD = Node.get<CXXRecordDecl>()) {
        if (::clang::isTemplateInstantiation(
                RD->getTemplateSpecializationKind())) {
            return TemplateMaterializationKind::Instantiation;
        }
        if (RD->getDescribedClassTemplate() || RD->isDependentContext()) {
            return TemplateMaterializationKind::UninstantiatedPattern;
        }
    }

    if (const auto* VD = Node.get<VarDecl>()) {
        if (const auto* Specialization =
                dyn_cast<VarTemplateSpecializationDecl>(VD)) {
            if (::clang::isTemplateInstantiation(
                    Specialization->getSpecializationKind())) {
                return TemplateMaterializationKind::Instantiation;
            }
        }
        if (VD->getDescribedVarTemplate()) {
            return TemplateMaterializationKind::UninstantiatedPattern;
        }
    }

    if (Node.get<FunctionTemplateDecl>() || Node.get<ClassTemplateDecl>() ||
        Node.get<VarTemplateDecl>()) {
        return TemplateMaterializationKind::UninstantiatedPattern;
    }

    return TemplateMaterializationKind::NonTemplate;
}

} // namespace

TemplateMaterializationKind classifyTemplateMaterialization(
    const DynTypedNode& Node, ASTContext& Context) {
    llvm::SmallVector<DynTypedNode, 8> Worklist;
    llvm::SmallPtrSet<const void*, 16> Seen;
    Worklist.push_back(Node);

    bool FoundPattern = false;
    while (!Worklist.empty()) {
        const DynTypedNode Current = Worklist.pop_back_val();
        const TemplateMaterializationKind CurrentKind = classifyOneNode(Current);
        if (CurrentKind == TemplateMaterializationKind::Instantiation) {
            return CurrentKind;
        }
        FoundPattern |=
            CurrentKind == TemplateMaterializationKind::UninstantiatedPattern;

        for (const DynTypedNode& Parent : Context.getParents(Current)) {
            const void* Identity = Parent.getMemoizationData();
            if (!Identity || Seen.insert(Identity).second) {
                Worklist.push_back(Parent);
            }
        }
    }

    return FoundPattern ? TemplateMaterializationKind::UninstantiatedPattern
                        : TemplateMaterializationKind::NonTemplate;
}

llvm::SmallVector<const Decl*, 2> collectTemplateInstantiationContexts(
    const DynTypedNode& Node, ASTContext& Context) {
    llvm::SmallVector<DynTypedNode, 8> Worklist;
    llvm::SmallPtrSet<const void*, 16> Seen;
    llvm::SmallPtrSet<const Decl*, 4> SeenInstantiations;
    llvm::SmallVector<const Decl*, 2> Instantiations;
    Worklist.push_back(Node);

    while (!Worklist.empty()) {
        const DynTypedNode Current = Worklist.pop_back_val();
        if (classifyOneNode(Current) ==
            TemplateMaterializationKind::Instantiation) {
            if (const auto* D = Current.get<Decl>()) {
                if (SeenInstantiations.insert(D).second) {
                    Instantiations.push_back(D);
                }
            }
            // This is the nearest instantiation boundary on this parent path.
            // An enclosing class/function instantiation is part of the same
            // concrete analysis instance, not another diagnostic identity.
            continue;
        }

        for (const DynTypedNode& Parent : Context.getParents(Current)) {
            const void* Identity = Parent.getMemoizationData();
            if (!Identity || Seen.insert(Identity).second) {
                Worklist.push_back(Parent);
            }
        }
    }

    return Instantiations;
}

bool AnalysisInstanceTracker::KeyLess::operator()(const Key& Left,
                                                  const Key& Right) const {
    if (Left.WrittenLocation != Right.WrittenLocation) {
        return Left.WrittenLocation < Right.WrittenLocation;
    }
    if (Left.ExpansionLocation != Right.ExpansionLocation) {
        return Left.ExpansionLocation < Right.ExpansionLocation;
    }
    return std::less<const Decl*>{}(Left.TemplateInstantiation,
                                    Right.TemplateInstantiation);
}

llvm::SmallVector<const Decl*, 2> AnalysisInstanceTracker::claim(
    const DynTypedNode& Node, SourceLocation Anchor, ASTContext& Context) {
    llvm::SmallVector<const Decl*, 2> Result;
    const SourceManager& SM = Context.getSourceManager();
    const SourceLocation Written = getUltimateWrittenLocation(Anchor, SM);
    if (Written.isInvalid() || !isWrittenInAnalyzedSource(Anchor, SM)) {
        return Result;
    }

    llvm::SmallVector<const Decl*, 2> Candidates;
    switch (classifyTemplateMaterialization(Node, Context)) {
    case TemplateMaterializationKind::NonTemplate:
        Candidates.push_back(nullptr);
        break;
    case TemplateMaterializationKind::UninstantiatedPattern:
        return Result;
    case TemplateMaterializationKind::Instantiation:
        Candidates = collectTemplateInstantiationContexts(Node, Context);
        break;
    }

    const unsigned ExpansionLocation =
        SM.getExpansionLoc(Anchor).getRawEncoding();
    for (const Decl* Candidate : Candidates) {
        const Key Identity{Written.getRawEncoding(), ExpansionLocation,
                           Candidate};
        if (Claimed.insert(Identity).second) {
            Result.push_back(Candidate);
        }
    }
    return Result;
}

bool isInMaterializedCode(const DynTypedNode& Node, ASTContext& Context) {
    return classifyTemplateMaterialization(Node, Context) !=
           TemplateMaterializationKind::UninstantiatedPattern;
}

SourceLocation getUltimateWrittenLocation(SourceLocation Location,
                                          const SourceManager& SM) {
    if (Location.isInvalid()) {
        return Location;
    }

    // Tokens produced by ## are spelled in Clang's scratch buffer.  When the
    // token still has macro expansion information, walk towards the caller
    // until its spelling identifies the source that supplied the paste.  A
    // direct scratch-buffer location has no such provenance and is left for
    // isWrittenInAnalyzedSource() to reject.
    while (Location.isMacroID() &&
           SM.isWrittenInScratchSpace(SM.getSpellingLoc(Location))) {
        const SourceLocation Caller = SM.getImmediateMacroCallerLoc(Location);
        if (Caller.isInvalid() || Caller == Location) {
            break;
        }
        Location = Caller;
    }
    return SM.getSpellingLoc(Location);
}

bool isWrittenInAnalyzedSource(SourceLocation Location,
                               const SourceManager& SM) {
    // SourceManager knows how to look through scratch buffers created by ##.
    // This rejects a pasted macro-body token from a system header while still
    // allowing a user-written argument passed through a system macro.
    if (SM.isInSystemMacro(Location)) {
        return false;
    }

    const SourceLocation Written = getUltimateWrittenLocation(Location, SM);
    if (Written.isInvalid() || SM.isWrittenInBuiltinFile(Written) ||
        SM.isWrittenInScratchSpace(Written) || SM.isInSystemHeader(Written)) {
        return false;
    }

    // A valid raw SourceLocation is not necessarily a printable source
    // location: compiler-created buffers can lack a presumed file identity.
    // Such nodes cannot identify a violation in targeted source code.
    const PresumedLoc Presumed = SM.getPresumedLoc(Written);
    return Presumed.isValid() && Presumed.getFilename()[0] != '\0';
}

bool isInAnalyzedCode(const DynTypedNode& Node, SourceLocation Anchor,
                      ASTContext& Context) {
    return isWrittenInAnalyzedSource(Anchor, Context.getSourceManager()) &&
           isInMaterializedCode(Node, Context);
}

} // namespace sdc
} // namespace tidy
} // namespace clang
