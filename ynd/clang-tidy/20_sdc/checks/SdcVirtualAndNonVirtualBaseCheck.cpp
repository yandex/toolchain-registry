#include "SdcVirtualAndNonVirtualBaseCheck.h"
#include "SdcCodeSelection.h"

#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace sdc {

namespace {

struct BaseKinds {
    bool Virtual = false;
    bool NonVirtual = false;
};

const CXXRecordDecl* definedRecord(QualType Type) {
    const CXXRecordDecl* Record = Type->getAsCXXRecordDecl();
    return Record && Record->hasDefinition() ? Record->getDefinition() : nullptr;
}

// Collect every direct derivation edge in Root's hierarchy. The rule compares
// direct virtual and non-virtual derivations of the same base, even when those
// edges occur in different intermediate branches.
void collectBaseKinds(const CXXRecordDecl* Record,
                      llvm::DenseMap<const CXXRecordDecl*, BaseKinds>& Kinds,
                      llvm::DenseSet<const CXXRecordDecl*>& Visited) {
    if (!Record || !Visited.insert(Record).second) {
        return;
    }

    for (const CXXBaseSpecifier& Specifier : Record->bases()) {
        const CXXRecordDecl* Base = definedRecord(Specifier.getType());
        if (!Base) {
            continue;
        }
        BaseKinds& Kind = Kinds[Base->getCanonicalDecl()];
        if (Specifier.isVirtual()) {
            Kind.Virtual = true;
        } else {
            Kind.NonVirtual = true;
        }
        collectBaseKinds(Base, Kinds, Visited);
    }
}

// A base is accessible to a member of Root when at least one path to it has no
// private derivation below Root. Root's own direct private base is accessible
// to Root's members, so the first edge intentionally does not block the path.
void collectAccessibleBases(const CXXRecordDecl* Record, unsigned Depth,
                            bool PathAccessible,
                            llvm::DenseSet<const CXXRecordDecl*>& Accessible,
                            llvm::DenseSet<const CXXRecordDecl*>& RecursionPath) {
    if (!Record || !RecursionPath.insert(Record).second) {
        return;
    }

    for (const CXXBaseSpecifier& Specifier : Record->bases()) {
        const CXXRecordDecl* Base = definedRecord(Specifier.getType());
        if (!Base) {
            continue;
        }

        const bool EdgeAccessible =
            Depth == 0 || Specifier.getAccessSpecifier() != AS_private;
        const bool BaseAccessible = PathAccessible && EdgeAccessible;
        if (BaseAccessible) {
            Accessible.insert(Base->getCanonicalDecl());
        }
        collectAccessibleBases(Base, Depth + 1, BaseAccessible, Accessible,
                               RecursionPath);
    }

    RecursionPath.erase(Record);
}

} // namespace

SdcVirtualAndNonVirtualBaseCheck::SdcVirtualAndNonVirtualBaseCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void SdcVirtualAndNonVirtualBaseCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        cxxRecordDecl(isDefinition(), unless(isImplicit()),
                      unless(isExpansionInSystemHeader()))
            .bind("record"),
        this);
}

void SdcVirtualAndNonVirtualBaseCheck::check(
    const MatchFinder::MatchResult& Result) {
    const auto* Record = Result.Nodes.getNodeAs<CXXRecordDecl>("record");
    if (!Record || Record->bases().empty() ||
        !isInAnalyzedCode(*Record, Record->getLocation(), *Result.Context)) {
        return;
    }

    llvm::DenseMap<const CXXRecordDecl*, BaseKinds> Kinds;
    llvm::DenseSet<const CXXRecordDecl*> Visited;
    collectBaseKinds(Record, Kinds, Visited);

    llvm::DenseSet<const CXXRecordDecl*> Accessible;
    llvm::DenseSet<const CXXRecordDecl*> RecursionPath;
    collectAccessibleBases(Record, 0, true, Accessible, RecursionPath);

    for (const auto& Entry : Kinds) {
        const CXXRecordDecl* Base = Entry.first;
        const BaseKinds& Kind = Entry.second;
        if (!Kind.Virtual || !Kind.NonVirtual || !Accessible.contains(Base)) {
            continue;
        }

        for (const Decl* Instance : AnalysisInstances.claim(
                 *Record, Record->getLocation(), *Result.Context)) {
            (void)Instance;
            diag(Record->getLocation(),
                 "class '%0' has accessible base class '%1' through both virtual "
                 "and non-virtual derivations")
                << Record->getName() << Base->getName();
        }
        return; // One diagnostic is sufficient to identify this hierarchy.
    }
}

} // namespace sdc
} // namespace tidy
} // namespace clang
