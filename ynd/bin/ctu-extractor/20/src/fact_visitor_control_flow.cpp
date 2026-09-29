// Compact Clang CFG extraction for CTU C++:2023 Rule 9.6.4.

#include "fact_visitor.h"

#include <algorithm>
#include <memory>
#include <set>

#include "clang/Analysis/CFG.h"

#include "source_utils.h"

namespace {

bool isExceptionalExitStmt(const Stmt *S) {
  if (!S) return false;
  if (isa<CXXThrowExpr>(S) || isa<ObjCAtThrowStmt>(S)) return true;
  if (auto *E = dyn_cast<Expr>(S))
    return isa<CXXThrowExpr>(E->IgnoreParenImpCasts());
  return false;
}

}  // namespace

void FactVisitor::emitControlFlow(FunctionDecl *FD,
                                  const facts_pb::EntityId &fn) {
  if (!FD || !FD->getBody()) return;

  CFG::BuildOptions options;
  // Calls through ordinary automatic-object destruction are part of normal
  // control flow. They are retained by Clang's graph even though the baseline
  // only joins source-backed call expressions today.
  options.AddImplicitDtors = true;
  std::unique_ptr<CFG> cfg = CFG::buildCFG(FD, FD->getBody(), &Ctx, options);
  if (!cfg) return;

  const CFGBlock *entry = &cfg->getEntry();
  const CFGBlock *exit = &cfg->getExit();
  for (const CFGBlock *block : *cfg) {
    if (!block || block == exit) continue;

    facts_pb::Fact fact;
    facts_pb::ControlFlowBlock *out = fact.mutable_control_flow_block();
    *out->mutable_function() = fn;
    out->set_block_id(block->getBlockID());
    out->set_is_entry(block == entry);

    std::set<uint32_t> successors;
    bool reachesExit = false;
    for (CFGBlock::const_succ_iterator it = block->succ_begin();
         it != block->succ_end(); ++it) {
      const CFGBlock *successor = it->getReachableBlock();
      if (!successor) continue;
      if (successor == exit) {
        reachesExit = true;
      } else {
        successors.insert(successor->getBlockID());
      }
    }
    for (uint32_t successor : successors) out->add_successors(successor);

    std::set<std::pair<uint64_t, uint64_t>> calls;
    bool exceptionalExit = false;
    const ReturnStmt *returnStmt = nullptr;
    for (const CFGElement &element : *block) {
      std::optional<CFGStmt> cfgStmt = element.getAs<CFGStmt>();
      if (!cfgStmt) continue;
      const Stmt *stmt = cfgStmt->getStmt();
      if (auto *RS = dyn_cast_or_null<ReturnStmt>(stmt)) returnStmt = RS;
      if (isExceptionalExitStmt(stmt)) exceptionalExit = true;
      if (auto *expr = dyn_cast_or_null<Expr>(stmt)) {
        auto found = callSiteNodes_.find(expr);
        if (found != callSiteNodes_.end()) calls.insert(found->second);
      }
    }
    for (const auto &call : calls) {
      facts_pb::ControlFlowCall *ref = out->add_calls();
      ref->set_full_expr_id(call.first);
      ref->set_node_id(call.second);
    }

    // Clang connects normal return, escaping throw, and known no-return sinks
    // to its synthetic EXIT block. Keep only normal return/fallthrough here.
    bool normalExit = reachesExit && !block->hasNoReturnElement() &&
                      !exceptionalExit;
    out->set_has_normal_exit(normalExit);
    if (normalExit) {
      SourceLocation loc = returnStmt ? returnStmt->getReturnLoc()
                                      : FD->getBody()->getEndLoc();
      setLoc(out->mutable_normal_exit_loc(), loc, SM);
    }
    fact.set_origin(originOf(isSystemLoc(FD->getLocation(), SM)));
    g_facts.push_back(std::move(fact));
  }
}
