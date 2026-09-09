#pragma once
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include <functional>

namespace clang::tidy::sdc::local_evidence {
// Visit evaluated syntax, excluding discarded branches and nested bodies.
// Unknown runtime branches are included unless LinearOnly is requested.
void walk(const Stmt *, ASTContext &, const std::function<void(const Stmt *)> &,
          bool LinearOnly = false);
bool visibleEvaluation(const Expr *, ASTContext &);
// A direct named object is sufficient evidence; pointer/reference targets are
// deliberately not inferred by this small syntactic helper.
const VarDecl *directObject(const Expr *);
bool constantBool(const Expr *, ASTContext &, bool &);
} // namespace clang::tidy::sdc::local_evidence
