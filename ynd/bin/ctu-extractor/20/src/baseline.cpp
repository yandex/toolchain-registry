#include "baseline.h"
#include "source_utils.h"
#include "initialization_flow.h"

void emitBaselineCandidate(clang::ASTContext &ctx, llvm::StringRef rule,
                           clang::SourceLocation loc, llvm::StringRef reason,
                           bool coverageGap, const clang::Expr *read) {
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
  if (rule == "11.6.2" && reason == "value-read-needs-initialization-proof")
    tagInitializationRead(read, candidate);
  g_facts.push_back(std::move(fact));
}
