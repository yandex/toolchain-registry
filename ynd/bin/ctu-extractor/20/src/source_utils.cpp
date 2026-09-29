// source_utils.cpp — see source_utils.h.

#include "source_utils.h"

#include "clang/Index/USRGeneration.h"

#include <memory>

#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/ODRHash.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

std::vector<facts_pb::Fact> g_facts;
std::string g_sourceRoot;

namespace {

// Clang's USR for a function-template pattern deliberately omits associated
// constraints. Consequently, two valid overloads that differ only in their
// constraints (including constrained deduction guides) receive the same USR.
// Add a semantic discriminator for the part the USR leaves out.
//
// Hash the constraint expressions themselves rather than the complete
// TemplateParameterList: the latter observes template-parameter spellings and
// would give equivalent redeclarations such as `template<C T>` and
// `template<C U>` different identities. ODRHash's statement hashing refers to
// template parameters by structural position, so those redeclarations remain
// identical while different concepts/requires-clauses remain distinct.
//
// Retain the structural origin of each expression as well. In particular,
// `template<C T>` and `template<class T> requires C<T>` are separate overloads
// in Clang even though hashing only the flattened associated-constraint list
// would make both lists identical.
unsigned constraintHash(const Expr *constraint) {
  ODRHash hash;
  hash.AddStmt(constraint);
  return hash.CalculateHash();
}

void appendTemplateConstraints(const TemplateParameterList *parameters,
                               StringRef path, llvm::raw_ostream &os,
                               bool *found) {
  for (unsigned i = 0; i < parameters->size(); ++i) {
    const NamedDecl *parameter = parameters->getParam(i);
    llvm::SmallVector<const Expr *, 1> constraints;
    if (const auto *type = dyn_cast<TemplateTypeParmDecl>(parameter)) {
      type->getAssociatedConstraints(constraints);
    } else if (const auto *value =
                   dyn_cast<NonTypeTemplateParmDecl>(parameter)) {
      value->getAssociatedConstraints(constraints);
    } else if (const auto *nested =
                   dyn_cast<TemplateTemplateParmDecl>(parameter)) {
      std::string nestedPath = (path + ".n" + llvm::Twine(i)).str();
      appendTemplateConstraints(nested->getTemplateParameters(), nestedPath,
                                os, found);
    }
    for (unsigned j = 0; j < constraints.size(); ++j) {
      os << path << ".p" << i << '.' << j << '='
         << constraintHash(constraints[j]) << ';';
      *found = true;
    }
  }
  if (const Expr *requires = parameters->getRequiresClause()) {
    os << path << ".r=" << constraintHash(requires) << ';';
    *found = true;
  }
}

std::string addConstraintDiscriminator(const FunctionDecl *FD,
                                       StringRef usr) {
  std::string suffix;
  llvm::raw_string_ostream os(suffix);
  bool found = false;
  if (const auto *FTD = FD->getDescribedFunctionTemplate())
    appendTemplateConstraints(FTD->getTemplateParameters(), "t", os, &found);
  llvm::SmallVector<const Expr *, 1> trailing;
  FD->getAssociatedConstraints(trailing);
  for (unsigned i = 0; i < trailing.size(); ++i) {
    os << "f.r" << i << '=' << constraintHash(trailing[i]) << ';';
    found = true;
  }
  os.flush();
  if (!found) return std::string(usr);
  return (usr + "@ctu-associated-constraints:" + suffix).str();
}

}  // namespace

std::string typeToString(QualType qt, ASTContext &ctx) {
  QualType canonical = ctx.getCanonicalType(qt);
  PrintingPolicy pp(ctx.getLangOpts());
  pp.SuppressTagKeyword = true;
  pp.Bool = true;
  return canonical.getAsString(pp);
}

void typeFingerprint(QualType qt, ASTContext &ctx, uint64_t *lo,
                     uint64_t *hi) {
  // Diagnostic printing is not a stable structural identity. In particular,
  // equivalent non-type template arguments can print as either an enumerator
  // name or its numeric value depending on instantiation state. The Itanium
  // type-name encoding is defined over the canonical AST structure and also
  // retains function-type properties such as noexcept and calling convention.
  std::string canonical = mangleType(qt, ctx);
  llvm::ArrayRef<uint8_t> ref(
      reinterpret_cast<const uint8_t *>(canonical.data()), canonical.size());
  std::array<uint8_t, 32> digest = llvm::SHA256::hash(ref);
  auto read64 = [&](unsigned offset) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
      value = (value << 8) | digest[offset + i];
    return value;
  };
  *lo = read64(0);
  *hi = read64(8);
}

void odrDefinitionFingerprint(const Decl *decl, ASTContext &, uint64_t *lo,
                              uint64_t *hi) {
  ODRHash odr;
  if (auto *record = dyn_cast<CXXRecordDecl>(decl)) {
    odr.AddCXXRecordDecl(record);
  } else if (auto *record = dyn_cast<RecordDecl>(decl)) {
    odr.AddRecordDecl(record);
  } else if (auto *function = dyn_cast<FunctionDecl>(decl)) {
    odr.AddFunctionDecl(function);
  } else if (auto *enumeration = dyn_cast<EnumDecl>(decl)) {
    odr.AddEnumDecl(enumeration);
  } else {
    odr.AddDecl(decl);
    if (auto *variable = dyn_cast<VarDecl>(decl)) {
      odr.AddQualType(variable->getType());
      if (variable->hasInit()) odr.AddStmt(variable->getInit());
    }
  }
  // ODRHash is Clang's purpose-built, pointer-independent semantic hash for
  // diagnosing ODR mismatches across modules.  Do not mix Decl::print output
  // into it: pretty-printing is not an ODR token stream and can include
  // context-dependent AST details, producing mismatches for one header parsed
  // in otherwise equivalent translation units.
  *lo = odr.CalculateHash();
  *hi = 0;
}

std::string normFile(SourceLocation loc, SourceManager &SM) {
  if (loc.isInvalid()) return "unknown";
  auto ploc = SM.getPresumedLoc(loc);
  if (!ploc.getFilename()) return "<builtin>";
  llvm::SmallString<256> abs(ploc.getFilename());
  llvm::sys::fs::make_absolute(abs);
  llvm::sys::path::remove_dots(abs, /*remove_dot_dot=*/true);
  if (!g_sourceRoot.empty()) {
    llvm::SmallString<256> root(g_sourceRoot);
    llvm::sys::fs::make_absolute(root);
    llvm::sys::path::remove_dots(root, true);
    llvm::StringRef a = abs.str();
    if (a.starts_with(root.str())) {
      llvm::StringRef rel = a.drop_front(root.size());
      while (rel.starts_with("/")) rel = rel.drop_front();
      return std::string(rel);
    }
  }
  return std::string(abs.str());
}

uint32_t lineOnly(SourceLocation loc, SourceManager &SM) {
  if (loc.isInvalid()) return 0;
  return SM.getPresumedLoc(loc).getLine();
}

std::string tuIdentity(SourceManager &SM) {
  return normFile(SM.getLocForStartOfFile(SM.getMainFileID()), SM);
}

bool isSystemLoc(SourceLocation loc, SourceManager &SM) {
  return loc.isValid() && SM.isInSystemHeader(loc);
}

void setLoc(facts_pb::SourceRef *ref, SourceLocation loc, SourceManager &SM) {
  ref->set_file(normFile(loc, SM));
  ref->set_line(lineOnly(loc, SM));
}

std::string mangleName(NamedDecl *ND, ASTContext &Ctx) {
  static ASTContext *cachedCtx = nullptr;
  static std::unique_ptr<MangleContext> cachedMC;
  if (cachedCtx != &Ctx) {
    cachedMC.reset(Ctx.createMangleContext());
    cachedCtx = &Ctx;
  }
  MangleContext *MC = cachedMC.get();
  // An uninstantiated function-template pattern has no unique linkage symbol.
  // Clang's Itanium mangler can return the same symbol as a non-template
  // overload (for example `T *Allocate<T>(size_t)` and
  // `void *Allocate(size_t)`). Prefer the semantic USR before consulting
  // shouldMangleDeclName so those different declarations never fuse.
  if (auto *FD = dyn_cast<FunctionDecl>(ND);
      FD && (FD->getTemplatedKind() == FunctionDecl::TK_FunctionTemplate ||
             FD->isDependentContext())) {
    llvm::SmallString<256> usr;
    if (!clang::index::generateUSRForDecl(FD, usr) && !usr.empty())
      return addConstraintDiscriminator(FD, usr);
  }
  if (!MC || !MC->shouldMangleDeclName(ND)) {
    return ND->getNameAsString();
  }
  std::string mangled;
  llvm::raw_string_ostream os(mangled);
  if (auto *CD = dyn_cast<CXXConstructorDecl>(ND))
    MC->mangleName(GlobalDecl(CD, Ctor_Complete), os);
  else if (auto *DD = dyn_cast<CXXDestructorDecl>(ND))
    MC->mangleName(GlobalDecl(DD, Dtor_Complete), os);
  else if (auto *FD = dyn_cast<FunctionDecl>(ND))
    MC->mangleName(GlobalDecl(FD), os);
  else if (auto *VD = dyn_cast<VarDecl>(ND))
    MC->mangleName(VD, os);
  else
    return ND->getNameAsString();
  os.flush();
  return mangled;
}

std::string mangleType(QualType QT, ASTContext &Ctx) {
  static ASTContext *cachedCtx = nullptr;
  static std::unique_ptr<MangleContext> cachedMC;
  if (cachedCtx != &Ctx) {
    cachedMC.reset(Ctx.createMangleContext());
    cachedCtx = &Ctx;
  }
  std::string s;
  llvm::raw_string_ostream os(s);
  cachedMC->mangleCXXRTTIName(Ctx.getCanonicalType(QT), os);
  os.flush();
  if (!s.empty()) return s;
  return typeToString(QT, Ctx);
}

uint64_t bodyHash(FunctionDecl *FD, ASTContext &Ctx) {
  if (!FD->doesThisDeclarationHaveABody()) return 0;
  Stmt *body = FD->getBody();
  SourceManager &SM = Ctx.getSourceManager();
  CharSourceRange range = CharSourceRange::getTokenRange(body->getSourceRange());
  bool invalid = false;
  StringRef text = Lexer::getSourceText(range, SM, Ctx.getLangOpts(), &invalid);
  if (invalid || text.empty()) return 0;
  return ctu::sha256Low64(text);
}

void makeEid(facts_pb::EntityId *eid, ctu::EntityKindTag kind,
             StringRef mangled, bool tuQualified, StringRef tuIdentity,
             std::string *keyOut) {
  std::string key = ctu::canonicalKey(kind, mangled, tuQualified, tuIdentity);
  eid->set_id(ctu::sha256Low64(key));
  eid->set_id_hi(0);
  if (keyOut) *keyOut = std::move(key);
}

void computeVarEntityId(VarDecl *VD, ASTContext &Ctx, SourceManager &SM,
                        facts_pb::EntityId *out) {
  makeEid(out, ctu::KIND_VAR, mangleName(VD, Ctx),
          /*tuQualified=*/!VD->isExternallyVisible(), tuIdentity(SM), nullptr);
}

void computeFuncEntityId(FunctionDecl *FD, ASTContext &Ctx,
                          SourceManager &SM, facts_pb::EntityId *out) {
  makeEid(out, ctu::KIND_FUNCTION, mangleName(FD, Ctx),
          /*tuQualified=*/!FD->isExternallyVisible(), tuIdentity(SM), nullptr);
}

facts_pb::Local *rootLocalOf(facts_pb::Aml *aml) {
  while (aml->has_field()) aml = aml->mutable_field()->mutable_base();
  return aml->has_local() ? aml->mutable_local() : nullptr;
}

facts_pb::OriginClass originOf(bool isSystem) {
  // T0.1 seam: only system-vs-not is derivable here without build metadata;
  // contrib classification lands with T0.8's policy config.
  return isSystem ? facts_pb::OC_SYSTEM : facts_pb::OC_FIRST_PARTY;
}
