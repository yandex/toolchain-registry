#include "baseline.h"
#include "source_utils.h"

void emitBaselineCandidate(clang::ASTContext &ctx, llvm::StringRef rule,
                           clang::SourceLocation loc, llvm::StringRef reason,
                           bool coverageGap) {
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
  g_facts.push_back(std::move(fact));
}
