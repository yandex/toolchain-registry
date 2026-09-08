#pragma once

#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/SmallVector.h"

#include <set>

namespace clang {
namespace tidy {
namespace sdc {

// Describes whether an AST node is ordinary code, belongs only to an
// uninstantiated template pattern, or participates in at least one concrete
// template instantiation. Explicit specializations are ordinary user-written
// code, rather than template instantiations, for this classification.
enum class TemplateMaterializationKind {
    NonTemplate,
    UninstantiatedPattern,
    Instantiation,
};

TemplateMaterializationKind classifyTemplateMaterialization(
    const DynTypedNode& Node, ASTContext& Context);

// Return the nearest concrete template-instantiation declarations reachable on
// all AST parent paths. A shared template-pattern node can have more than one
// such context when several specializations instantiate the same written
// construct. Callers that emit per-specialization diagnostics use these
// declarations as stable identities; they must not deduplicate only by source
// location.
llvm::SmallVector<const Decl*, 2> collectTemplateInstantiationContexts(
    const DynTypedNode& Node, ASTContext& Context);

// Tracks which concrete analysis instances have already been claimed for a
// written construct. AST matchers can visit one shared template-pattern node
// once for the pattern and again for every specialization. claim() converts
// those visits into exactly one instance for ordinary code or one instance per
// concrete specialization. A null Decl denotes ordinary non-template code.
class AnalysisInstanceTracker {
public:
    llvm::SmallVector<const Decl*, 2> claim(const DynTypedNode& Node,
                                            SourceLocation Anchor,
                                            ASTContext& Context);

    template <typename NodeT>
    llvm::SmallVector<const Decl*, 2> claim(const NodeT& Node,
                                            SourceLocation Anchor,
                                            ASTContext& Context) {
        return claim(DynTypedNode::create(Node), Anchor, Context);
    }

private:
    struct Key {
        unsigned WrittenLocation;
        unsigned ExpansionLocation;
        const Decl* TemplateInstantiation;
    };

    struct KeyLess {
        bool operator()(const Key& Left, const Key& Right) const;
    };

    std::set<Key, KeyLess> Claimed;
};

// POL-MAT-001 default: ordinary code and instantiated templates are analyzed;
// nodes belonging only to dormant template patterns are not.
bool isInMaterializedCode(const DynTypedNode& Node, ASTContext& Context);

// POL-SRC-001/POL-MAC-001: determine scope from the ultimate location where
// the relevant token was written. For a macro argument this is the argument;
// for a macro-body token this is the macro definition.
SourceLocation getUltimateWrittenLocation(SourceLocation Location,
                                          const SourceManager& SM);
bool isWrittenInAnalyzedSource(SourceLocation Location,
                               const SourceManager& SM);

// Default combined policy for AST-based checks. The checker supplies the
// location of the token that constitutes the prohibited construct; choosing
// that rule-specific anchor cannot be inferred safely by this component.
bool isInAnalyzedCode(const DynTypedNode& Node, SourceLocation Anchor,
                      ASTContext& Context);

template <typename NodeT>
bool isInMaterializedCode(const NodeT& Node, ASTContext& Context) {
    return isInMaterializedCode(DynTypedNode::create(Node), Context);
}

template <typename NodeT>
bool isInAnalyzedCode(const NodeT& Node, SourceLocation Anchor,
                      ASTContext& Context) {
    return isInAnalyzedCode(DynTypedNode::create(Node), Anchor, Context);
}

} // namespace sdc
} // namespace tidy
} // namespace clang
