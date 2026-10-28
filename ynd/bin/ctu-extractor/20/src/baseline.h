#ifndef CTU_EXTRACTOR_BASELINE_H
#define CTU_EXTRACTOR_BASELINE_H

#include "clang/AST/ASTContext.h"
#include "llvm/ADT/StringRef.h"

void emitBaselineCandidate(clang::ASTContext &ctx, llvm::StringRef rule,
                           clang::SourceLocation loc, llvm::StringRef reason,
                           bool coverageGap = false, const clang::Expr *read = nullptr,
                           const clang::FunctionDecl *effectCallee = nullptr);
// Resolve only direct calls or immutable function pointers initialized by an
// exact function address. Mutable/externally supplied target sets stay unknown.
const clang::FunctionDecl *exactFunctionPointerTarget(const clang::VarDecl *var);
const clang::FunctionDecl *exactDirectCallee(const clang::CallExpr *call);
void collectMemoryBaselineFacts(clang::ASTContext &ctx);
void collectSemanticBaselineFacts(clang::ASTContext &ctx);

#endif
