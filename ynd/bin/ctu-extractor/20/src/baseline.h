#ifndef CTU_EXTRACTOR_BASELINE_H
#define CTU_EXTRACTOR_BASELINE_H

#include "clang/AST/ASTContext.h"
#include "llvm/ADT/StringRef.h"

void emitBaselineCandidate(clang::ASTContext &ctx, llvm::StringRef rule,
                           clang::SourceLocation loc, llvm::StringRef reason,
                           bool coverageGap = false);
void collectMemoryBaselineFacts(clang::ASTContext &ctx);
void collectSemanticBaselineFacts(clang::ASTContext &ctx);

#endif
