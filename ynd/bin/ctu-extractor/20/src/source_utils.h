// source_utils.h — shared entity-naming/location helpers and the global fact
// collector, used by both FactVisitor (fact_visitor_*.cpp) and the driver
// (driver.cpp). See entity_id.h for the entity-ID scheme these build on.

#ifndef CTU_EXTRACTOR_SOURCE_UTILS_H
#define CTU_EXTRACTOR_SOURCE_UTILS_H

#include <cstdint>
#include <string>
#include <vector>

#include "clang/AST/ASTContext.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/StringRef.h"

#include "factspb/facts.pb.h"
#include "entity_id.h"

using namespace clang;
namespace facts_pb = ctu::facts::v2;

// ---------------------------------------------------------------------------
// Global collector. One TU per process, so a plain global is fine.
// ---------------------------------------------------------------------------
extern std::vector<facts_pb::Fact> g_facts;
extern std::string g_sourceRoot;

// ---------------------------------------------------------------------------
// Ported helpers (see legacy/ctu_extract.cpp for the original rationale).
// ---------------------------------------------------------------------------

std::string typeToString(QualType qt, ASTContext &ctx);

// Compact canonical type identity for cross-TU declaration compatibility.
// Both halves are the first 128 bits of SHA-256 over Clang's structural
// Itanium type-name encoding (with a canonical-name fallback).
void typeFingerprint(QualType qt, ASTContext &ctx, uint64_t *lo,
                     uint64_t *hi);

// Stable 128-bit definition identity for CTU 6.2.1. Combines Clang's AST
// ODRHash (which observes semantic lookup/type differences) with normalized
// declaration printing (which raises the collision strength to SHA-256/128).
void odrDefinitionFingerprint(const Decl *decl, ASTContext &ctx, uint64_t *lo,
                              uint64_t *hi);

// Presumed-loc filename (honors #line), normalized to source-root-relative so
// blobs are reproducible across machines and the entity-ID TU-qualifier is
// stable. THE single path choke-point.
std::string normFile(SourceLocation loc, SourceManager &SM);

uint32_t lineOnly(SourceLocation loc, SourceManager &SM);

// The entity-ID TU qualifier: main file of the TU, never the defining file
// (see entity_id.h — qualifying by defining file fuses header-defined
// internal-linkage entities across TUs).
std::string tuIdentity(SourceManager &SM);

bool isSystemLoc(SourceLocation loc, SourceManager &SM);

void setLoc(facts_pb::SourceRef *ref, SourceLocation loc, SourceManager &SM);

// Itanium mangling with ctor/dtor complete-object special-casing and a
// per-ASTContext cache (one TU per process). Uninstantiated function-template
// patterns, which have no linkage symbol, use Clang's semantic USR plus an
// associated-constraint discriminator (Clang omits constraints from USRs) so
// overloads never collapse to the same source spelling.
std::string mangleName(NamedDecl *ND, ASTContext &Ctx);

// Mangled type identity (the `_ZTS...` type-string mangling), the stable ID
// pre-image for a type entity. Falls back to the canonical type name.
std::string mangleType(QualType QT, ASTContext &Ctx);

// body_hash: SHA-256 low64 of the body's raw token text (NOT the old,
// version-unstable llvm::hash_value). Uses doesThisDeclarationHaveABody().
uint64_t bodyHash(FunctionDecl *FD, ASTContext &Ctx);

// Assign an EntityId (and stash the canonical pre-image in *keyOut for the
// analyzer's collision self-check).
void makeEid(facts_pb::EntityId *eid, ctu::EntityKindTag kind,
             StringRef mangled, bool tuQualified, StringRef tuIdentity,
             std::string *keyOut);

// The same EntityId VisitVarDecl assigns a global VarDecl — shared so a
// NamedGlobal Aml referring to it, computed at some other reference site,
// names the identical entity rather than a recomputed/possibly-diverging one.
void computeVarEntityId(VarDecl *VD, ASTContext &Ctx, SourceManager &SM,
                         facts_pb::EntityId *out);

// Same idea as computeVarEntityId, for FunctionDecl (shared with
// VisitFunctionDecl's own id assignment so a CallSite's callee id matches).
void computeFuncEntityId(FunctionDecl *FD, ASTContext &Ctx,
                          SourceManager &SM, facts_pb::EntityId *out);

// Unwraps Field wrappers down to the root Local, if any (used for the
// escape retroactive-fixup — a Field-wrapped access's escaped bit lives on
// the nested Local inside field.base, not on the Aml directly).
facts_pb::Local *rootLocalOf(facts_pb::Aml *aml);

facts_pb::OriginClass originOf(bool isSystem);

#endif  // CTU_EXTRACTOR_SOURCE_UTILS_H
