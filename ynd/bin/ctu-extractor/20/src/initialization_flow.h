#ifndef CTU_EXTRACTOR_INITIALIZATION_FLOW_H
#define CTU_EXTRACTOR_INITIALIZATION_FLOW_H
#include "source_utils.h"
void collectInitializationCallers(clang::ASTContext &ctx);
void emitInitializationFlow(clang::FunctionDecl *function,
                            clang::ASTContext &ctx,
                            const facts_pb::EntityId &id);
void tagInitializationRead(const clang::Expr *expr,
                           facts_pb::BaselineCandidate *candidate);
#endif
