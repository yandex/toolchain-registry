#include "baseline.h"
#include "clang/AST/DeclCXX.h"
#include "source_utils.h"
#include "initialization_flow.h"

void emitBaselineCandidate(clang::ASTContext &ctx, llvm::StringRef rule,
                           clang::SourceLocation loc, llvm::StringRef reason,
                           bool coverageGap, const clang::Expr *read,
                           const clang::FunctionDecl *effectCallee) {
  auto &sm = ctx.getSourceManager();
  // Preserve otherwise unlocatable obligations at the translation-unit root.
  if (loc.isInvalid()) {
    loc = sm.getLocForStartOfFile(sm.getMainFileID());
    coverageGap = true;
  }
  facts_pb::Fact fact;
  fact.set_origin(originOf(isSystemLoc(loc, sm)));
  auto *candidate = fact.mutable_baseline_candidate();
  candidate->set_rule_id(rule.str());
  setLoc(candidate->mutable_loc(), loc, sm);
  candidate->set_reason(reason.str());
  candidate->set_coverage_gap(coverageGap);
  if (rule == "11.6.2" && (reason == "value-read-needs-initialization-proof" ||
                           reason == "read-modify-write-needs-initialization-proof" ||
                           reason == "aggregate-copy-needs-member-initialization-proof"))
    tagInitializationRead(read, candidate);
  if (effectCallee) {
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(effectCallee);
    if (!method || !method->isVirtual())
      computeFuncEntityId(const_cast<clang::FunctionDecl *>(effectCallee), ctx, sm,
                          candidate->mutable_effect_callee());
  }
  g_facts.push_back(std::move(fact));
}

const clang::FunctionDecl *exactFunctionPointerTarget(const clang::VarDecl *var) {
  if (!var || !var->getType().isConstQualified() || var->getType().isVolatileQualified() ||
      !var->getType()->isFunctionPointerType() || !var->hasInit()) return nullptr;
  const auto *expr = var->getInit()->IgnoreParenImpCasts();
  if (const auto *address = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
    if (address->getOpcode() != clang::UO_AddrOf) return nullptr;
    expr = address->getSubExpr()->IgnoreParenImpCasts();
  }
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr);
  return ref ? llvm::dyn_cast<clang::FunctionDecl>(ref->getDecl()) : nullptr;
}
const clang::FunctionDecl *exactDirectCallee(const clang::CallExpr *call) {
  if (!call) return nullptr;
  if (const auto *direct = call->getDirectCallee()) return direct;
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(call->getCallee()->IgnoreParenImpCasts());
  return ref ? exactFunctionPointerTarget(llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) : nullptr;
}
