#ifndef CTU_EXTRACTOR_INITIALIZATION_FLOW_H
#define CTU_EXTRACTOR_INITIALIZATION_FLOW_H
#include "source_utils.h"
#include "llvm/ADT/DenseMap.h"
#include <utility>
using InitializationCallSites = llvm::DenseMap<const clang::Expr *, std::pair<uint64_t, uint64_t>>;
using InitializationAccessSites = llvm::DenseMap<std::pair<const clang::Expr *, unsigned>, std::pair<uint64_t, uint64_t>>;
void collectInitializationCallers(clang::ASTContext &ctx);
void emitInitializationFlow(clang::FunctionDecl *function,
                            clang::ASTContext &ctx,
                            const facts_pb::EntityId &id,
                            const InitializationCallSites &callSites,
                            const InitializationAccessSites &accessSites);
void tagInitializationRead(const clang::Expr *expr,
                           facts_pb::BaselineCandidate *candidate);
void tagPointerOriginRead(const clang::Expr *expr, facts_pb::BaselineCandidate *candidate);
#endif
