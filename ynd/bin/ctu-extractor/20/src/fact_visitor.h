// fact_visitor.h — the FactVisitor RecursiveASTVisitor declaration. Method
// bodies (with their full documentation) live in fact_visitor_decls.cpp,
// fact_visitor_exprs.cpp and fact_visitor_pointers.cpp, grouped by
// responsibility: decl visitation, full-expression/access emission, and
// pointer resolution/call-site emission respectively.

#ifndef CTU_EXTRACTOR_FACT_VISITOR_H
#define CTU_EXTRACTOR_FACT_VISITOR_H

#include <cstdint>
#include <utility>
#include <vector>

#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include "factspb/facts.pb.h"

using namespace clang;
namespace facts_pb = ctu::facts::v2;

class FactVisitor : public RecursiveASTVisitor<FactVisitor> {
public:
  explicit FactVisitor(ASTContext &Ctx) : Ctx(Ctx), SM(Ctx.getSourceManager()) {}

  bool shouldVisitTemplateInstantiations() const { return true; }

  // Without this, RecursiveASTVisitor never visits compiler-synthesized
  // special members at all (default ctor, copy/move ctor, copy/move
  // assignment, dtor) — not just their bodies, the DECLARATIONS themselves.
  // A CallSite to one is still emitted (the CALLER'S AST node is real), but
  // with no corresponding FuncDecl fact anywhere, it looks identical to a
  // genuinely undefined/external function: full Widening, every time,
  // for what is usually a trivial member-wise copy/move/destroy. This was
  // the single largest non-widening-related cause bucket in a real run
  // (1145/3567 findings — "undefined callee (no FuncDecl fact at all)").
  bool shouldVisitImplicitCode() const { return true; }

  // See fact_visitor_decls.cpp.
  bool VisitCXXRecordDecl(CXXRecordDecl *RD);
  bool VisitEnumDecl(EnumDecl *ED);
  bool VisitVarDecl(VarDecl *VD);
  bool VisitFunctionDecl(FunctionDecl *FD);

private:
  void emitOdrDefinition(const Decl *D, const facts_pb::EntityId &entity,
                         facts_pb::EntityKind kind, StringRef name,
                         bool allowsMultiple, bool isSystem,
                         StringRef entityKey, bool completeIdentity = true);

  // --- fact_visitor_exprs.cpp: full-expression/access emission ---------

  // Find full-expressions (statement-level expressions) and build a node tree
  // for each. Deliberately simple: good enough to exercise the schema.
  void emitFullExprs(Stmt *S, const facts_pb::EntityId &fn);

  // Emit the compact normal-control-flow graph consumed by Rule 9.6.4.
  // Call expressions are joined to CallSite facts through callSiteNodes_.
  void emitControlFlow(FunctionDecl *FD, const facts_pb::EntityId &fn);

  // readModifyWrite: the child is both read and written (compound assign,
  // ++/--) — emit both Access facts for it, not just the WRITE.
  struct Kid {
    Expr *E;
    bool writeCtx;
    bool readModifyWrite = false;
    // A NON-volatile glvalue cast to void (`(void)x;`, the common unused-
    // variable-silencing idiom) performs no lvalue-to-rvalue conversion at
    // all (see hasLValueToRValueConversion's own comment) — so without
    // this flag, buildNode's "neither read nor written" fallback (below)
    // would misinterpret a pure discard as reference-binding/pass-by-
    // reference and wrongly mark the underlying local escaped. A discard
    // exposes nothing: no address taken, no reference bound.
    bool discarded = false;
  };

  // Does E's glvalue chain (stripping only parens/implicit casts) undergo an
  // lvalue-to-rvalue conversion on the way to the underlying lvalue? That
  // conversion IS the real read; its absence (reference-binding, address-of,
  // member-access-as-base) means no access happens no matter how
  // volatile-shaped the lvalue looks. A discarded-value `(void)vol_obj;`
  // still carries this cast (the standard preserves volatile semantics
  // there), so it's correctly still a read.
  static bool hasLValueToRValueConversion(Expr *E);

  // Returns this expression's node id. Emits an ExprNode for it (and an Access
  // if it is a volatile lvalue). Parens/implicit casts are transparent.
  uint64_t buildNode(Expr *E, const facts_pb::EntityId &fn, uint64_t feid,
                     bool writeCtx, bool readModifyWrite = false,
                     bool discarded = false);

  // Ask Clang for ordinary final/fixed dispatch first, then apply the C++
  // construction/destruction rule for calls on the current `this` object:
  // dynamic dispatch is limited to the class whose ctor/dtor is executing.
  CXXMethodDecl *devirtualizedMethod(CXXMethodDecl *method,
                                     Expr *receiver) const;

  // Record a complete polymorphic object's dynamic type for conservative
  // whole-program instantiated-type refinement of virtual dispatch.
  void emitObjectConstruction(CXXConstructExpr *E,
                              const facts_pb::EntityId *fn = nullptr);

  // Emits an exact dynamic-type seed when E's object origin is provable.
  // The Aml has already been resolved in the surrounding assignment/call.
  CXXRecordDecl *exactDynamicClass(Expr *E) const;
  void emitReceiverTypeSeed(const facts_pb::EntityId &fn, const facts_pb::Aml &object,
                            Expr *E);

  // Record one Rule 15.1.1 operation in the current function's AML space.
  // Pointer operands name their pointee; glvalue operands name the object.
  void emitDynamicTypeUse(Expr *object, const facts_pb::EntityId &fn,
                          facts_pb::DynamicTypeUse::Kind kind,
                          SourceLocation loc);

  // *p where p points to volatile is also a volatile lvalue.
  static bool isVolatileLvalue(Expr *E);

  // Recursively finds the root VarDecl of a dot-access field chain
  // (a.b.c -> a), or of a plain DeclRefExpr (a -> a). Never crosses `->`
  // (the base would be a pointer dereference, out of scope — see
  // computeNamedVarAml). Used for escape tracking: taking the address of,
  // or reference-binding, a field exposes its whole containing object.
  static VarDecl *rootVarOfFieldChain(Expr *E);

  // &local (or &local.field) escapes the containing local (a call elsewhere
  // could then write through that pointer) — globals don't need this, a
  // call can already plausibly touch any global regardless (see mayOverlap
  // in the Go checker).
  void markEscapeIfLocal(Expr *E);

  // A local (never a parameter, never global/static) pointer variable whose
  // assignments we can observe within this one function — the scope of
  // today's per-function-only Steensgaard-lite unification (no
  // cross-function summary merging yet; see DESIGN.md §4). Parameters are
  // deliberately excluded: what they might point to is a cross-function
  // question (the schema's ParamRegion is reserved for that, not attempted
  // here).
  static bool isTrackablePointerLocal(const VarDecl *VD);

  // If E (after stripping) is a DeclRefExpr to a trackable pointer local,
  // returns it; else nullptr.
  static VarDecl *trackablePointerVarOf(Expr *E);

  // Resolves E to a "region reachable through parameter N" (schema's
  // ParamRegion): what a pointer/reference PARAMETER points to — a
  // cross-function question we don't try to unify (unlike local pointers),
  // just name so it stops being invisible to the checker. depth counts
  // dereferences from the parameter's own value: a bare reference parameter
  // used directly is depth 1 (references are transparent aliases of their
  // referent, no `*` in the AST); `*p`/`p[i]` on a pointer parameter is also
  // depth 1; further derefs (`**pp`) accumulate depth via recursion. A bare
  // pointer parameter with no deref at all (depth 0) is NOT a ParamRegion —
  // that's the parameter's own local storage, already covered by the
  // ordinary Local path below.
  //
  // `this` (CXXThisExpr — not a DeclRefExpr/ParmVarDecl at all, so it would
  // otherwise never match any of the below) is treated exactly like a bare
  // pointer parameter at the reserved index currentFunctionParamCount_: a
  // method almost always writes to its own object via `this->member` (or
  // the implicit-this form, `member = ...`), and until this was recognized
  // that was completely invisible to the whole analysis — not even
  // widened, silently absent from own-effects.
  bool paramRegionOf(Expr *E, unsigned &index, unsigned &depth);

  // --- fact_visitor_pointers.cpp: pointer resolution / call-site emission

  uint32_t getOrAssignUnifyClassId(const VarDecl *VD);

  // Returns a synthetic UnifyClass id representing a given function's
  // return value.  Used both by the callee's own R_RETURN (to name its
  // own return class) AND by callers on the RHS of `p = callee()` (so
  // the caller's pointer class gets unioned with the same synthetic id).
  //
  // CRITICAL: this ID lives in the SAME namespace as getOrAssignUnifyClassId
  // (nextUnifyClassId_), because every synthetic UnifyClass id ends up as an
  // opaque uint32 in the CALLER'S function scope (see the amlKey format
  // "U:<fn>:<class_id>") — if the two counters were independent, a caller's
  // own local pointer p (say id=0) would COLLIDE with the synthetic
  // return-class for some callee it happens to call first (also id=0),
  // silently unioning p with unrelated things.
  uint32_t getOrAssignReturnClassId(const facts_pb::EntityId &fnId);

  // Returns true if the given QualType is a pointer or reference type
  // (including function pointers).
  bool isPointerType(QualType QT) const;

  // Returns true if FD is a known allocator function (malloc, calloc,
  // realloc, mmap, operator new — the set of functions whose return value
  // is a fresh, non-aliasing pointer).  The name check is deliberately
  // simple: these are the C/C++ standard names; a user-defined wrapper
  // around malloc is NOT recognized here (its transitive footprint will
  // pick up malloc's AllocatorLoc write, but the returned pointer's
  // AllocationId won't propagate — a future refinement).
  static bool isKnownAllocator(FunctionDecl *FD);

  // Returns true if FD is the glibc/musl errno accessor (`__errno_location`,
  // which the `errno` macro expands to). Its return value's pointee is the
  // reserved-name synthetic NamedGlobal "__synthetic_errno_buf"
  // (analyzer/libc_stubs.go's synthErrnoBuf), not a generic UnifyClass.
  static bool isErrnoLocation(FunctionDecl *FD);

  // What E denotes as a pointer VALUE's target (one level of indirection):
  // `&x` -> x's own Aml; a trackable local pointer -> its UnifyClass; a
  // pointer/reference PARAMETER -> ParamRegion one level deeper. Shared by
  // pointer-assignment RHS resolution and CallSite arg resolution.
  bool resolvePointerTargetAml(Expr *E, facts_pb::Aml *aml);

  // A deliberately narrow model of std::function: an automatic
  // std::function object gets a function-pointer-like UnifyClass, populated
  // from direct construction/assignment below.  Calls through that object
  // can then use the ordinary bounded function-pointer resolver.  We do not
  // pretend to understand captures, type-erasure internals, or escaping
  // std::function objects; an unrecognised source simply taints the class.
  static bool isStdFunctionLocal(const VarDecl *VD);
  void handleStdFunctionAssignment(VarDecl *VD, Expr *RHS,
                                   const facts_pb::EntityId &fn);

  // Emits a UnifyConstraint for `VD = RHS` (VD already known trackable):
  // resolved RHS unifies VD's class with it; unresolvable RHS taints via
  // Widening — the analyzer's union-find does the actual merging.
  void handlePointerAssignment(VarDecl *VD, Expr *RHS, const facts_pb::EntityId &fn);

  // Resolves a member/operator() call's RECEIVER expression (the object the
  // call is invoked on) to what `this` equals inside the callee — the
  // counterpart, on the CALLER side, to paramRegionOf's CXXThisExpr case on
  // the callee side. For `->`, the receiver expression is already a pointer
  // VALUE: reuse resolvePointerTargetAml exactly like a regular pointer
  // argument (what it points to IS the object). For `.`, the receiver is
  // the object lvalue itself — there's no pointer to dereference, it's
  // already the object — resolved directly via computeNamedVarAml, as if
  // an implicit `&receiver` had been taken.
  bool computeReceiverAml(Expr *E, bool isArrow, facts_pb::Aml *aml);

  // Emits a CallSite fact (W3 call graph). calleeFD is null for
  // indirect/virtual calls (is_indirect=true, callee left unresolved — the
  // schema's stated convention). args are resolved via
  // resolvePointerTargetAml; unresolvable ones widen rather than being
  // omitted, so remap always has a positional entry to look up. virtualMD
  // is non-null iff this is specifically a virtual dispatch (a SUBSET of
  // indirect): unlike a raw function pointer, a virtual call's exact
  // statically-selected method slot and receiver static class are knowable
  // single-TU — the
  // analyzer's whole-program CHA (cha.go) resolves them to every override
  // that actually exists across every merged blob.
  //
  // receiverExpr/receiverIsArrow/methodForReceiver are for a member call's
  // implicit object argument (constructor receiver wiring is a separate,
  // not-yet-done follow-up — still falls back to widening, sound, just not
  // maximally precise yet). rawReceiver is an ALREADY-RESOLVED Aml used
  // instead of resolving an expression — for a temporary's destructor call,
  // which has no natural "expression" for its own identity the way a
  // member-call receiver does (see the CXXBindTemporaryExpr case, which
  // synthesizes a fresh Local). Exactly one of receiverExpr/rawReceiver is
  // set when methodForReceiver is; neither, otherwise. methodForReceiver's
  // own getNumParams() gives the RESERVED slot paramRegionOf's CXXThisExpr
  // case uses inside the callee — every override of a virtual method
  // shares the same signature by construction, so this stays correct even
  // when the actual dispatch target isn't known until the Go analyzer's
  // CHA pass.
  void emitCallSite(FunctionDecl *calleeFD, bool indirect,
                     const std::vector<Expr *> &args, const facts_pb::EntityId &fn,
                     uint64_t feid, uint64_t nodeId, SourceLocation loc,
                     CXXMethodDecl *virtualMD = nullptr,
                     Expr *receiverExpr = nullptr, bool receiverIsArrow = false,
                     CXXMethodDecl *methodForReceiver = nullptr,
                     const facts_pb::Aml *rawReceiver = nullptr,
                     const Expr *sourceCall = nullptr,
                     Expr *calleeExpr = nullptr);

  // Named-variable/field/pointer-deref/param-region Amls (see DESIGN.md §4).
  // Array subscript by a constant index reuses the Field/field_path
  // machinery: distinct constant indices of one region never overlap.
  bool computeNamedVarAml(Expr *E, facts_pb::Aml *aml);

  ASTContext &Ctx;
  SourceManager &SM;
  uint64_t fullExprCounter_ = 0;
  uint64_t nodeCounter_ = 0;
  llvm::DenseMap<const VarDecl *, uint32_t> localIds_;
  // Escape tracking (this function only; reset per function): locals whose
  // address was taken or that were reference-bound/passed-by-reference
  // anywhere, and the g_facts indices of their already-emitted Access facts
  // needing a retroactive escaped=true fixup (see VisitFunctionDecl).
  llvm::DenseSet<const VarDecl *> escapedLocals_;
  llvm::DenseMap<const VarDecl *, std::vector<size_t>> localAccessFactIdx_;
  // Steensgaard-lite unification (this function only — see DESIGN.md §4;
  // no cross-function summary merging yet): per-function class ids for
  // local pointer variables, assigned like localIds_.
  llvm::DenseMap<const VarDecl *, uint32_t> unifyClassIds_;
  // Per-function return-value class ids: each function that returns a
  // pointer gets a synthetic UnifyClass representing "the return value."
  // When the function has `return <expr>;` where <expr> is a pointer, a
  // UnifyConstraint with R_RETURN connects this class to whatever <expr>
  // resolves to.  The cross-function unification pass then connects the
  // caller's pointer at call sites to this resolved target.
  //
  // Shares its numeric namespace with unifyClassIds_ (both allocated from
  // nextUnifyClassId_) — see getOrAssignReturnClassId's comment.
  llvm::DenseMap<uint64_t, uint32_t> returnClassIds_;
  // Shared allocator for BOTH unifyClassIds_ and returnClassIds_ values —
  // every synthetic UnifyClass id lives in the SAME caller-scoped
  // "U:<fn>:<id>" namespace on the analyzer side, so their id spaces
  // MUST NOT overlap.
  uint32_t nextUnifyClassId_ = 0;
  // Pending "this call's return goes to this pointer local" links, keyed
  // by the CallExpr the extractor is about to visit next. Populated in
  // handlePointerAssignment when the RHS is a call, consumed (and erased)
  // in emitCallSite. Reset per function.
  llvm::DenseMap<const Expr *, uint32_t> pendingCallSiteReturnClassId_;
  // Source call expression -> (full-expression id, expression node id).
  // Populated by emitCallSite while the ordinary AST walk runs, then consumed
  // by emitControlFlow to attach resolved calls to their Clang CFG blocks.
  llvm::DenseMap<const Expr *, std::pair<uint64_t, uint64_t>> callSiteNodes_;
  // `this`'s reserved ParamRegion index for the function currently being
  // walked (see paramRegionOf's CXXThisExpr case) — one past its own real
  // parameters, reset per function alongside the other per-function state
  // above.
  unsigned currentFunctionParamCount_ = 0;
  // Non-null only while buildNode walks one function's body/ctor-initializers.
  // Used to recognize calls on the current object during construction and
  // destruction; never used to devirtualize calls on some other object.
  FunctionDecl *currentFunctionDecl_ = nullptr;
  // Fresh, non-escaping Local id for the NEXT temporary needing a
  // synthesized destructor-call receiver (see CXXBindTemporaryExpr) —
  // offset well above any real local's id (localIds_ starts at 0) to
  // guarantee no collision within one function.
  uint32_t nextTempLocalId_ = 0x80000000u;
  // Per-function allocation-site counter: each distinct call site to a
  // known allocator (malloc, calloc, realloc, mmap, operator new) gets a
  // deterministic, function-scoped id — the Go analyzer's AllocationId.
  // Reset per function alongside the other per-function state above.
  uint32_t allocIdCounter_ = 0;
  // Deferred escape tracking: maps a local whose address was taken
  // (&local) to the local pointer variable that received it (p = &local).
  // At function end, the transitive escape closure is computed: if the
  // pointer variable itself never escapes, the local whose address it
  // holds doesn't either — un-escape it. This avoids the over-
  // approximation where &local marks local as escaped even when the
  // address only flows to a non-escaping local pointer.
  llvm::DenseMap<const VarDecl *, const VarDecl *> addrTakenTo_;
  // A pointer assigned addresses of multiple locals is ambiguous: even when
  // the pointer itself stays local, dereferencing it may reach any recorded
  // local, so none of those targets can be treated as non-escaping.
  llvm::DenseSet<const VarDecl *> addrTakenAmbiguousPtrs_;
};

#endif  // CTU_EXTRACTOR_FACT_VISITOR_H
