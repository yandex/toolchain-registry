#pragma once
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Expr.h"
namespace clang::tidy::sdc::expression_detail {
const Expr *peel(const Expr *);
DynTypedNode semanticParent(const Expr *, ASTContext &);
bool constant(const Expr *, ASTContext &, llvm::APSInt &);
}
