// fact_visitor_decls.cpp — FactVisitor's decl-visitation methods: class,
// variable and function declarations (schema v2 ClassInfo/VarDecl/FuncDecl
// facts, plus the per-function full-expression walk kickoff).

#include "fact_visitor.h"

#include <algorithm>
#include <functional>
#include <vector>

#include "clang/AST/EvaluatedExprVisitor.h"
#include "clang/AST/ExprConcepts.h"

#include "source_utils.h"

namespace {
bool allowsMultipleOdrDefinitions(const FunctionDecl *FD) {
  TemplateSpecializationKind specialization =
      FD->getTemplateSpecializationKind();
  return FD->isInlined() ||
         ((FD->isTemplated() ||
           FD->getTemplatedKind() != FunctionDecl::TK_NonTemplate) &&
          specialization != TSK_ExplicitSpecialization &&
          specialization != TSK_ExplicitInstantiationDefinition);
}

bool allowsMultipleOdrDefinitions(const VarDecl *VD) {
  if (VD->isInline()) return true;

  TemplateSpecializationKind specialization =
      VD->getTemplateSpecializationKind();
  if (specialization == TSK_ExplicitSpecialization ||
      specialization == TSK_ExplicitInstantiationDefinition)
    return false;
  if (VD->isTemplated()) return true;

  // A static data member of a class template is a templated entity even
  // though VarDecl::isTemplated() is false for its out-of-class definition.
  // Likewise, a block static inside an inline or templated function denotes
  // the one entity shared by all definitions of that function.
  for (const DeclContext *DC = VD->getDeclContext(); DC;
       DC = DC->getParent()) {
    if (DC->isDependentContext()) return true;
    if (const auto *RD = dyn_cast<CXXRecordDecl>(DC)) {
      if (RD->getDescribedClassTemplate()) return true;
      if (const auto *specialization =
              dyn_cast<ClassTemplateSpecializationDecl>(RD))
        if (specialization->getSpecializationKind() !=
            TSK_ExplicitSpecialization)
          return true;
    }
    if (const auto *FD = dyn_cast<FunctionDecl>(DC))
      return allowsMultipleOdrDefinitions(FD);
    if (DC->isTranslationUnit() || DC->isNamespace()) break;
  }
  return false;
}

class GlobalConstructionCollector
    : public EvaluatedExprVisitor<GlobalConstructionCollector> {
 public:
  explicit GlobalConstructionCollector(ASTContext &Ctx)
      : EvaluatedExprVisitor(Ctx) {}

  void VisitCXXConstructExpr(CXXConstructExpr *E) {
    constructions.push_back(E);
    VisitExpr(E);
  }

  std::vector<CXXConstructExpr *> constructions;
};
}  // namespace

void FactVisitor::emitOdrDefinition(const Decl *D,
                                    const facts_pb::EntityId &entity,
                                    facts_pb::EntityKind kind, StringRef name,
                                    bool allowsMultiple, bool isSystem,
                                    StringRef entityKey,
                                    bool completeIdentity) {
  facts_pb::Fact fact;
  fact.set_origin(originOf(isSystem));
  facts_pb::OdrDefinition *definition = fact.mutable_odr_definition();
  *definition->mutable_entity() = entity;
  definition->set_entity_kind(kind);
  definition->set_name(name.str());
  definition->set_allows_multiple_definitions(allowsMultiple);
  uint64_t hashLo = 0, hashHi = 0;
  odrDefinitionFingerprint(D, Ctx, &hashLo, &hashHi);
  definition->set_definition_hash(hashLo);
  definition->set_definition_hash_hi(hashHi);
  setLoc(definition->mutable_loc(), D->getLocation(), SM);
  definition->set_translation_unit(tuIdentity(SM));
  definition->set_entity_key(entityKey.str());
  definition->set_is_system_header(isSystem);
  definition->set_source_language(Ctx.getLangOpts().CPlusPlus ? facts_pb::SL_CXX
                                                               : facts_pb::SL_C);
  definition->set_has_complete_identity(completeIdentity);
  g_facts.push_back(std::move(fact));
}

bool FactVisitor::VisitCXXRecordDecl(CXXRecordDecl *RD) {
  if (!RD->isThisDeclarationADefinition()) return true;
  // getName() asserts Name.isIdentifier() — lambdas, anonymous structs,
  // and other compiler-synthesized types have non-identifier names.
  if (!RD->getDeclName().isIdentifier() || RD->getName().empty()) return true;
  if (RD->isImplicit()) return true;

  bool sys = isSystemLoc(RD->getLocation(), SM);
  std::string key;
  QualType T = Ctx.getRecordType(RD);
  std::string typeIdentity = mangleType(T, Ctx);
  facts_pb::EntityId typeID;
  makeEid(&typeID, ctu::KIND_TYPE, typeIdentity,
          /*tuQualified=*/!RD->isExternallyVisible(), tuIdentity(SM), &key);

  bool emitDefinition = true;
  if (auto *specialization = dyn_cast<ClassTemplateSpecializationDecl>(RD)) {
    emitDefinition = specialization->getSpecializationKind() !=
                     TSK_ImplicitInstantiation;
  }
  // Internal-linkage entities are TU-qualified distinct entities. They cannot
  // violate the cross-TU ODR join, and duplicate definitions inside one TU
  // are rejected by the frontend before a complete blob exists.
  bool completeDefinitionIdentity = true;
  for (const CXXMethodDecl *method : RD->methods()) {
    if (!method->isExplicitlyDefaulted()) continue;
    const FunctionProtoType *type = method->getType()->getAs<FunctionProtoType>();
    if (!type) continue;
    ExceptionSpecificationType exception = type->getExceptionSpecType();
    if (exception == EST_Unevaluated || exception == EST_Uninstantiated ||
        exception == EST_DependentNoexcept) {
      completeDefinitionIdentity = false;
      break;
    }
  }
  if (emitDefinition && RD->isExternallyVisible()) {
    emitOdrDefinition(RD, typeID, facts_pb::EK_TYPE, RD->getName(),
                      /*allowsMultiple=*/true, sys, key,
                      completeDefinitionIdentity);
  }

  // Non-polymorphic template instantiations (the overwhelming majority —
  // containers, smart pointers, etc.) never participate in CHA, so they're
  // still skipped entirely to bound fact volume. A POLYMORPHIC instantiation
  // (e.g. a template class deriving from a non-template base with a virtual
  // method) does need a ClassInfo, or cha.go can never find its vtable —
  // this was the previously-deferred gap (W3's class-template CHA blind
  // spot).
  if (isa<ClassTemplateSpecializationDecl>(RD) && !RD->isPolymorphic())
    return true;

  facts_pb::Fact fact;
  facts_pb::ClassInfo *ci = fact.mutable_class_info();
  fact.set_origin(originOf(sys));

  *ci->mutable_id() = typeID;
  ci->set_entity_key(key);
  ci->set_mangled(typeIdentity);
  ci->set_name(RD->getNameAsString());
  for (auto &B : RD->bases())
    if (B.getType()->getAsCXXRecordDecl())
      ci->add_bases(mangleType(B.getType(), Ctx));
  std::vector<std::string> polymorphicMembers;
  for (const FieldDecl *Field : RD->fields()) {
    QualType memberType = Ctx.getBaseElementType(Field->getType());
    if (CXXRecordDecl *MemberRD = memberType->getAsCXXRecordDecl())
      if (MemberRD->isPolymorphic())
        polymorphicMembers.push_back(mangleType(memberType, Ctx));
  }
  std::sort(polymorphicMembers.begin(), polymorphicMembers.end());
  polymorphicMembers.erase(
      std::unique(polymorphicMembers.begin(), polymorphicMembers.end()),
      polymorphicMembers.end());
  for (const auto &member : polymorphicMembers)
    ci->add_polymorphic_member_classes(member);
  // virtual_methods sorted by mangled for stable serialization.
  std::vector<facts_pb::VMethod> vms;
  for (auto *M : RD->methods()) {
    if (!M->isVirtual()) continue;
    facts_pb::VMethod vm;
    vm.set_name(M->getNameAsString());
    vm.set_mangled(mangleName(M, Ctx));
    vm.set_is_pure(M->isPureVirtual());
    std::vector<std::string> overridden;
    for (auto *BaseMethod : M->overridden_methods())
      overridden.push_back(
          mangleName(const_cast<CXXMethodDecl *>(BaseMethod), Ctx));
    std::sort(overridden.begin(), overridden.end());
    for (const auto &name : overridden) vm.add_overridden_mangled(name);
    vms.push_back(std::move(vm));
  }
  std::sort(vms.begin(), vms.end(),
            [](const facts_pb::VMethod &a, const facts_pb::VMethod &b) {
              return a.mangled() < b.mangled();
            });
  for (auto &vm : vms) *ci->add_virtual_methods() = vm;
  ci->set_is_system_header(sys);
  ci->set_has_complete_object_construction_coverage(true);
  setLoc(ci->mutable_loc(), RD->getLocation(), SM);
  g_facts.push_back(std::move(fact));
  return true;
}

bool FactVisitor::VisitEnumDecl(EnumDecl *ED) {
  if (!Ctx.getLangOpts().CPlusPlus || !ED->isCompleteDefinition() ||
      ED->isImplicit() || !ED->getDeclName().isIdentifier() ||
      ED->getName().empty())
    return true;

  QualType type = Ctx.getEnumType(ED);
  std::string identity = mangleType(type, Ctx);
  std::string key;
  facts_pb::EntityId id;
  makeEid(&id, ctu::KIND_TYPE, identity,
          /*tuQualified=*/!ED->isExternallyVisible(), tuIdentity(SM), &key);
  if (ED->isExternallyVisible()) {
    emitOdrDefinition(ED, id, facts_pb::EK_TYPE, ED->getName(),
                      /*allowsMultiple=*/true,
                      isSystemLoc(ED->getLocation(), SM), key);
  }
  return true;
}

bool FactVisitor::VisitVarDecl(VarDecl *VD) {
  // hasGlobalStorage() excludes automatic locals; block-local statics
  // must still be extracted (entity_id.h claims this coverage).
  if (!VD->hasGlobalStorage()) return true;
  if (isa<VarTemplateSpecializationDecl>(VD)) return true;  // mangler crashes; not source-level

  facts_pb::Fact fact;
  facts_pb::VarDecl *v = fact.mutable_var_decl();
  bool sys = isSystemLoc(VD->getLocation(), SM);
  fact.set_origin(originOf(sys));

  bool external = VD->isExternallyVisible();
  std::string mangled = mangleName(VD, Ctx);
  std::string key;
  makeEid(v->mutable_id(), ctu::KIND_VAR, mangled, /*tuQualified=*/!external,
          tuIdentity(SM), &key);
  v->set_entity_key(key);
  v->set_mangled(mangled);
  v->set_name(VD->getNameAsString());
  v->set_type(typeToString(VD->getType(), Ctx));
  uint64_t varTypeLo = 0, varTypeHi = 0;
  typeFingerprint(VD->getType(), Ctx, &varTypeLo, &varTypeHi);
  v->set_canonical_type_hash(varTypeLo);
  v->set_canonical_type_hash_hi(varTypeHi);
  v->set_has_complete_type_identity(true);
  v->set_has_outer_array_compatibility(true);
  if (const ArrayType *arrayType = Ctx.getAsArrayType(VD->getType())) {
    uint64_t elementTypeLo = 0, elementTypeHi = 0;
    typeFingerprint(arrayType->getElementType(), Ctx, &elementTypeLo,
                    &elementTypeHi);
    v->set_is_outer_array(true);
    v->set_outer_array_bound_omitted(isa<IncompleteArrayType>(arrayType));
    v->set_outer_array_element_type_hash(elementTypeLo);
    v->set_outer_array_element_type_hash_hi(elementTypeHi);
  }
  v->set_source_language(Ctx.getLangOpts().CPlusPlus ? facts_pb::SL_CXX : facts_pb::SL_C);
  // Definition status tested BEFORE linkage (FN-critical).
  if (VD->isThisDeclarationADefinition() == VarDecl::Definition)
    v->set_kind(facts_pb::VarDecl::DEF);
  else if (VD->isThisDeclarationADefinition() == VarDecl::TentativeDefinition)
    v->set_kind(facts_pb::VarDecl::TENTATIVE_DEF);
  else if (VD->hasExternalFormalLinkage())
    v->set_kind(facts_pb::VarDecl::EXTERN);
  else
    v->set_kind(facts_pb::VarDecl::DECL);
  v->set_is_inline(VD->isInline());
  v->set_linkage(external ? facts_pb::LK_EXTERNAL : facts_pb::LK_INTERNAL);
  v->set_is_system_header(sys);
  setLoc(v->mutable_loc(), VD->getLocation(), SM);

  TemplateSpecializationKind variableSpecialization =
      VD->getTemplateSpecializationKind();
  if (Ctx.getLangOpts().CPlusPlus && external &&
      v->kind() == facts_pb::VarDecl::DEF && !VD->isImplicit() &&
      variableSpecialization != TSK_ImplicitInstantiation) {
    bool allowsMultiple = allowsMultipleOdrDefinitions(VD);
    emitOdrDefinition(VD, v->id(), facts_pb::EK_VAR, VD->getName(), allowsMultiple,
                      sys, key);
  }

  // Namespace/class-scope initialization is outside every FunctionDecl
  // body, so buildNode cannot see its constructions. EvaluatedExprVisitor
  // deliberately skips sizeof/noexcept and other unevaluated operands.
  // Static locals are excluded here: their initializer is visited by the
  // enclosing function's buildNode and must retain that function owner.
  if (!VD->isLocalVarDecl() && VD->hasInit()) {
    GlobalConstructionCollector collector(Ctx);
    collector.Visit(VD->getInit());
    for (CXXConstructExpr *construction : collector.constructions)
      emitObjectConstruction(construction);
  }

  // Global function-pointer initializers have no enclosing ExprNode/body,
  // so ordinary assignment handling never sees them. Record a concrete
  // target on the global's actual NamedGlobal storage for the indirect-call
  // resolver (for example, `static Hook hook = default_hook;`).
  if (VD->hasGlobalStorage() && VD->getType()->isFunctionPointerType() &&
      VD->hasInit()) {
    facts_pb::Aml rhsAml;
    if (resolvePointerTargetAml(VD->getInit(), &rhsAml) && rhsAml.has_func_target()) {
      facts_pb::Fact targetFact;
      facts_pb::UnifyConstraint *uc = targetFact.mutable_unify_constraint();
      *uc->mutable_a()->mutable_named_global()->mutable_entity() = v->id();
      *uc->mutable_b() = std::move(rhsAml);
      uc->set_reason(facts_pb::UnifyConstraint::R_ASSIGN);
      setLoc(uc->mutable_loc(), VD->getLocation(), SM);
      targetFact.set_origin(originOf(sys));
      g_facts.push_back(std::move(targetFact));
    }
  }

  // The same gap exists for a callback table with static storage: its
  // initializer is outside every function body, so assignment visitation
  // never observes `static Table table = { callback };`.  Record direct
  // function-pointer fields in a flat record initializer on their precise
  // Field(NamedGlobal, field-index) storage.  Nested aggregates and runtime
  // initialization deliberately remain ordinary expression handling; this
  // covers the common, exact callback-table declaration without inventing
  // targets for omitted or non-function-pointer fields.
  if (VD->hasInit()) {
    const RecordType *RT = VD->getType()->getAs<RecordType>();
    InitListExpr *ILE = dyn_cast<InitListExpr>(
        VD->getInit()->IgnoreParenImpCasts());
    if (RT && ILE) {
      const RecordDecl *RD = RT->getDecl();
      unsigned index = 0;
      for (const FieldDecl *FD : RD->fields()) {
        if (index >= ILE->getNumInits()) break;
        Expr *init = ILE->getInit(index++);
        if (!FD->getType()->isFunctionPointerType()) continue;

        facts_pb::Aml rhsAml;
        if (!resolvePointerTargetAml(init, &rhsAml) || !rhsAml.has_func_target())
          continue;

        facts_pb::Fact targetFact;
        facts_pb::UnifyConstraint *uc = targetFact.mutable_unify_constraint();
        facts_pb::Field *field = uc->mutable_a()->mutable_field();
        *field->mutable_base()->mutable_named_global()->mutable_entity() = v->id();
        field->add_field_path(FD->getFieldIndex());
        *uc->mutable_b() = std::move(rhsAml);
        uc->set_reason(facts_pb::UnifyConstraint::R_FUNC_PTR_STORE);
        setLoc(uc->mutable_loc(), VD->getLocation(), SM);
        targetFact.set_origin(originOf(sys));
        g_facts.push_back(std::move(targetFact));
      }
    }
  }

  // Likewise preserve direct targets in a top-level static array of function
  // pointers. computeNamedVarAml already represents a constant subscript as
  // Field(NamedGlobal, index), so emitting the same path here makes
  // `static Hook hooks[] = { first, second }; hooks[0](...)` exact without
  // guessing about non-constant indices or runtime writes.
  if (VD->hasInit()) {
    const ArrayType *AT = Ctx.getAsArrayType(VD->getType());
    InitListExpr *ILE = dyn_cast<InitListExpr>(
        VD->getInit()->IgnoreParenImpCasts());
    if (AT && AT->getElementType()->isFunctionPointerType() && ILE) {
      for (unsigned index = 0; index < ILE->getNumInits(); ++index) {
        facts_pb::Aml rhsAml;
        if (!resolvePointerTargetAml(ILE->getInit(index), &rhsAml) ||
            !rhsAml.has_func_target())
          continue;
        facts_pb::Fact targetFact;
        facts_pb::UnifyConstraint *uc = targetFact.mutable_unify_constraint();
        facts_pb::Field *field = uc->mutable_a()->mutable_field();
        *field->mutable_base()->mutable_named_global()->mutable_entity() = v->id();
        field->add_field_path(index);
        *uc->mutable_b() = std::move(rhsAml);
        uc->set_reason(facts_pb::UnifyConstraint::R_FUNC_PTR_STORE);
        setLoc(uc->mutable_loc(), VD->getLocation(), SM);
        targetFact.set_origin(originOf(sys));
        g_facts.push_back(std::move(targetFact));
      }
    }
  }

  // Arrays of callback records are selected through a runtime index in many
  // C-style dispatch tables (`table[i].callback(...)`). A dynamic subscript
  // intentionally drops the array index from the AML field path, so collect
  // every initializer for each callback field onto that generalized path.
  // Also retain the index-qualified path for constant-subscript users.
  if (VD->hasInit()) {
    const ArrayType *topArray = Ctx.getAsArrayType(VD->getType());
    InitListExpr *topInit = dyn_cast<InitListExpr>(
        VD->getInit()->IgnoreParenImpCasts());
    if (VD->hasGlobalStorage() && topArray &&
        topArray->getElementType()->getAs<RecordType>() && topInit) {
      auto emitTarget = [&](ArrayRef<unsigned> path, Expr *init) {
        facts_pb::Aml rhsAml;
        bool resolved = resolvePointerTargetAml(init, &rhsAml);
        bool isNull = !resolved && init->isNullPointerConstant(
            Ctx, Expr::NPC_ValueDependentIsNotNull) != Expr::NPCK_NotNull;
        if (isNull) return;

        facts_pb::Fact targetFact;
        facts_pb::UnifyConstraint *uc = targetFact.mutable_unify_constraint();
        facts_pb::Field *field = uc->mutable_a()->mutable_field();
        *field->mutable_base()->mutable_named_global()->mutable_entity() = v->id();
        for (unsigned index : path) field->add_field_path(index);
        if (resolved) {
          *uc->mutable_b() = std::move(rhsAml);
        } else {
          uc->mutable_b()->mutable_widening()->set_kind(
              facts_pb::Widening::W_UNKNOWN_EVERYTHING);
        }
        uc->set_reason(facts_pb::UnifyConstraint::R_FUNC_PTR_STORE);
        setLoc(uc->mutable_loc(), init->getExprLoc(), SM);
        targetFact.set_origin(originOf(sys));
        g_facts.push_back(std::move(targetFact));
      };

      std::function<void(QualType, Expr *, std::vector<unsigned>,
                         std::vector<unsigned>)>
          collectRecordTargets;
      collectRecordTargets = [&](QualType type, Expr *init,
                                 std::vector<unsigned> generalPath,
                                 std::vector<unsigned> exactPath) {
        init = init->IgnoreParenImpCasts();
        auto *ILE = dyn_cast<InitListExpr>(init);
        if (!ILE) return;
        if (const ArrayType *AT = Ctx.getAsArrayType(type)) {
          for (unsigned i = 0; i < ILE->getNumInits(); ++i) {
            std::vector<unsigned> elementPath = exactPath;
            elementPath.push_back(i);
            collectRecordTargets(AT->getElementType(), ILE->getInit(i),
                                 generalPath, std::move(elementPath));
          }
          return;
        }
        const RecordType *RT = type->getAs<RecordType>();
        if (!RT) return;
        if (const auto *CXXRD = dyn_cast<CXXRecordDecl>(RT->getDecl());
            CXXRD && CXXRD->getNumBases() != 0)
          return;
        unsigned initIndex = 0;
        for (const FieldDecl *FD : RT->getDecl()->fields()) {
          if (initIndex >= ILE->getNumInits()) break;
          Expr *fieldInit = ILE->getInit(initIndex++);
          std::vector<unsigned> fieldGeneral = generalPath;
          std::vector<unsigned> fieldExact = exactPath;
          fieldGeneral.push_back(FD->getFieldIndex());
          fieldExact.push_back(FD->getFieldIndex());
          if (FD->getType()->isFunctionPointerType()) {
            emitTarget(fieldGeneral, fieldInit);
            if (fieldExact != fieldGeneral) emitTarget(fieldExact, fieldInit);
          } else if (Ctx.getAsArrayType(FD->getType()) ||
                     FD->getType()->getAs<RecordType>()) {
            collectRecordTargets(FD->getType(), fieldInit,
                                 std::move(fieldGeneral),
                                 std::move(fieldExact));
          }
        }
      };
      collectRecordTargets(VD->getType(), topInit, {}, {});
    }
  }

  // A multidimensional callback table is commonly selected with runtime
  // indices and returned through a helper (for example Zstd's compressor
  // strategy table). A non-constant subscript intentionally resolves to the
  // whole named array region, so retain the union of every direct function
  // target on that base as well as any exact one-dimensional entries above.
  // Arrays are homogeneous: every callable leaf has the same function-pointer
  // type, making this a sound bounded candidate set rather than a cross-field
  // guess for arbitrary records.
  if (VD->hasInit()) {
    const ArrayType *AT = Ctx.getAsArrayType(VD->getType());
    InitListExpr *ILE = dyn_cast<InitListExpr>(
        VD->getInit()->IgnoreParenImpCasts());
    if (AT && Ctx.getAsArrayType(AT->getElementType()) && ILE) {
      std::function<void(Expr *)> collectTargets = [&](Expr *init) {
        init = init->IgnoreParenImpCasts();
        if (auto *nested = dyn_cast<InitListExpr>(init)) {
          for (Expr *child : nested->inits()) collectTargets(child);
          return;
        }
        facts_pb::Aml rhsAml;
        if (!resolvePointerTargetAml(init, &rhsAml) ||
            !rhsAml.has_func_target())
          return;
        facts_pb::Fact targetFact;
        facts_pb::UnifyConstraint *uc = targetFact.mutable_unify_constraint();
        *uc->mutable_a()->mutable_named_global()->mutable_entity() = v->id();
        *uc->mutable_b() = std::move(rhsAml);
        uc->set_reason(facts_pb::UnifyConstraint::R_FUNC_PTR_STORE);
        setLoc(uc->mutable_loc(), init->getExprLoc(), SM);
        targetFact.set_origin(originOf(sys));
        g_facts.push_back(std::move(targetFact));
      };
      collectTargets(ILE);
    }
  }
  g_facts.push_back(std::move(fact));
  return true;
}

bool FactVisitor::VisitFunctionDecl(FunctionDecl *FD) {
  bool sys = isSystemLoc(FD->getLocation(), SM);

  bool isTemplatePattern =
      FD->getTemplatedKind() == FunctionDecl::TK_FunctionTemplate ||
      FD->isDependentContext();

  // CTU 21.6.4: global operator delete/delete[] — usual replaceable forms
  // only (placement/nothrow forms would manufacture fake violations).
  if (!isa<CXXMethodDecl>(FD) && FD->isThisDeclarationADefinition()) {
    OverloadedOperatorKind op = FD->getOverloadedOperator();
    if (op == OO_Delete || op == OO_Array_Delete) {
      bool sized = false, aligned = false, other = false;
      for (unsigned pi = 1; pi < FD->getNumParams(); ++pi) {
        QualType t = Ctx.getCanonicalType(FD->getParamDecl(pi)->getType());
        if (t == Ctx.getCanonicalType(Ctx.getSizeType())) sized = true;
        else if (t->isAlignValT()) aligned = true;
        else other = true;
      }
      if (!other) {
        facts_pb::Fact gf;
        facts_pb::GlobalDelete *gd = gf.mutable_global_delete();
        gf.set_origin(originOf(sys));
        gd->set_kind(op == OO_Array_Delete ? facts_pb::GlobalDelete::DELETE_ARRAY
                                           : facts_pb::GlobalDelete::DELETE);
        gd->set_sized(sized);
        gd->set_aligned(aligned);
        gd->set_is_system_header(sys);
        setLoc(gd->mutable_loc(), FD->getLocation(), SM);
        g_facts.push_back(std::move(gf));
      }
    }
  }

  facts_pb::Fact fact;
  facts_pb::FuncDecl *f = fact.mutable_func_decl();
  fact.set_origin(originOf(sys));

  bool external = FD->isExternallyVisible();
  std::string mangled = mangleName(FD, Ctx);
  std::string key;
  makeEid(f->mutable_id(), ctu::KIND_FUNCTION, mangled,
          /*tuQualified=*/!external, tuIdentity(SM), &key);
  f->set_entity_key(key);
  f->set_mangled(mangled);
  f->set_name(FD->getNameAsString());
  f->set_ret_type(typeToString(FD->getReturnType(), Ctx));
  for (auto *P : FD->parameters()) {
    f->add_params(typeToString(P->getType(), Ctx));
    f->add_param_names(P->getNameAsString());
  }
  f->set_has_complete_override_identity(true);
  if (auto *method = dyn_cast<CXXMethodDecl>(FD)) {
    std::vector<facts_pb::EntityId> overriddenIds;
    overriddenIds.reserve(method->size_overridden_methods());
    for (const CXXMethodDecl *baseMethod : method->overridden_methods()) {
      facts_pb::EntityId id;
      computeFuncEntityId(const_cast<CXXMethodDecl *>(baseMethod), Ctx, SM,
                          &id);
      overriddenIds.push_back(std::move(id));
    }
    std::sort(overriddenIds.begin(), overriddenIds.end(),
              [](const facts_pb::EntityId &a, const facts_pb::EntityId &b) {
                if (a.id_hi() != b.id_hi()) return a.id_hi() < b.id_hi();
                return a.id() < b.id();
              });
    overriddenIds.erase(
        std::unique(overriddenIds.begin(), overriddenIds.end(),
                    [](const facts_pb::EntityId &a, const facts_pb::EntityId &b) {
                      return a.id() == b.id() && a.id_hi() == b.id_hi();
                    }),
        overriddenIds.end());
    for (auto &id : overriddenIds) *f->add_overridden_ids() = std::move(id);
  }
  // Clang evaluates implicit/defaulted exception specifications lazily. The
  // same explicitly defaulted source declaration can otherwise appear as
  // `void()` in one TU and `void() noexcept` in another solely because it was
  // odr-used in one of them. Forcing evaluation here is not semantics-neutral:
  // constrained constructors can instantiate deliberately ill-formed default
  // member initializers that normal compilation never needs. Mark unresolved
  // identities incomplete so the analyzer falls back within that entity.
  QualType fingerprintType = FD->getType();
  const FunctionProtoType *FPT =
      fingerprintType->getAs<FunctionProtoType>();
  bool completeTypeIdentity =
      !FPT || (FPT->getExceptionSpecType() != EST_Unevaluated &&
               FPT->getExceptionSpecType() != EST_Uninstantiated &&
               FPT->getExceptionSpecType() != EST_DependentNoexcept);
  // A placeholder return type and its later deduced type are two AST states
  // of one declaration, not two source declaration types. Keep the fact for
  // coverage telemetry but do not use it as mismatch evidence until deduced.
  if (FD->getReturnType()->isUndeducedType()) completeTypeIdentity = false;
  uint64_t functionTypeLo = 0, functionTypeHi = 0;
  typeFingerprint(fingerprintType, Ctx, &functionTypeLo, &functionTypeHi);
  f->set_canonical_type_hash(functionTypeLo);
  f->set_canonical_type_hash_hi(functionTypeHi);
  f->set_has_complete_type_identity(completeTypeIdentity);
  f->set_has_noreturn_attribute_identity(true);
  bool hasExplicitNoreturn = false;
  for (const Attr *attribute : FD->attrs()) {
    if (isa<CXX11NoReturnAttr>(attribute) && !attribute->isInherited()) {
      hasExplicitNoreturn = true;
      break;
    }
  }
  f->set_has_noreturn_attribute(hasExplicitNoreturn);
  f->set_source_language(Ctx.getLangOpts().CPlusPlus ? facts_pb::SL_CXX : facts_pb::SL_C);
  bool isDef = FD->isThisDeclarationADefinition();
  // Template instantiations don't carry their own body — it lives on
  // the template pattern (a different FunctionDecl, with a different
  // mangled name and thus a different entity id).  But semantically,
  // every implicit or explicit instantiation whose pattern has a body
  // IS a definition — missed, and the Go analyzer treats it as an
  // undefined callee (full Widening) even for pure-cast helpers like
  // std::move / std::forward that touch no memory at all.
  if (!isDef && FD->isTemplateInstantiation())
    if (auto *Pat = FD->getTemplateInstantiationPattern())
      isDef = Pat->doesThisDeclarationHaveABody();
  f->set_kind(isDef ? facts_pb::FuncDecl::DEF : facts_pb::FuncDecl::DECL);
  f->set_is_inline(FD->isInlined());
  f->set_is_templated(FD->getTemplatedKind() != FunctionDecl::TK_NonTemplate);
  f->set_is_template_pattern(isTemplatePattern);
  f->set_is_implicit(FD->isImplicit());
  f->set_is_lambda(isa<CXXMethodDecl>(FD) && dyn_cast<CXXMethodDecl>(FD)->getParent() && dyn_cast<CXXMethodDecl>(FD)->getParent()->isLambda());
  f->set_is_template_specialization(FD->isTemplateInstantiation() || FD->getTemplatedKind() == FunctionDecl::TK_MemberSpecialization);
  f->set_is_constructor(isa<CXXConstructorDecl>(FD));
  f->set_is_destructor(isa<CXXDestructorDecl>(FD));
  f->set_has_memory_effect_attribute_identity(true);
  f->set_has_const_attribute(FD->hasAttr<ConstAttr>());
  f->set_has_pure_attribute(FD->hasAttr<PureAttr>());
  f->set_has_complete_member_function_identity(true);
  f->set_is_nonstatic_member_function(
      isa<CXXMethodDecl>(FD) && !cast<CXXMethodDecl>(FD)->isStatic());
  if (auto *MD = dyn_cast<CXXMethodDecl>(FD)) {
    if (MD->isPureVirtual()) f->set_virtuality(facts_pb::FuncDecl::V_PURE);
    else if (MD->isVirtual()) f->set_virtuality(facts_pb::FuncDecl::V_VIRTUAL);
    if (auto *RD = MD->getParent())
      for (auto &B : RD->bases())
        if (auto *BaseRD = B.getType()->getAsCXXRecordDecl())
          f->add_base_classes(BaseRD->getNameAsString());
  }
  f->set_body_hash(bodyHash(FD, Ctx));
  f->set_linkage(external ? facts_pb::LK_EXTERNAL : facts_pb::LK_INTERNAL);
  f->set_is_system_header(sys);
  setLoc(f->mutable_loc(), FD->getLocation(), SM);
  facts_pb::EntityId fnId = f->id();

  TemplateSpecializationKind specialization =
      FD->getTemplateSpecializationKind();
  bool sourceDefinition = FD->isThisDeclarationADefinition() &&
                          !FD->isImplicit() &&
                          specialization != TSK_ImplicitInstantiation;
  if (Ctx.getLangOpts().CPlusPlus && external && sourceDefinition) {
    bool allowsMultiple = allowsMultipleOdrDefinitions(FD);
    emitOdrDefinition(FD, fnId, facts_pb::EK_FUNCTION, FD->getNameAsString(),
                      allowsMultiple, sys, key, completeTypeIdentity);
  }
  g_facts.push_back(std::move(fact));

  // --- Spine smoke: volatile Access + ExprNode facts over the body. ---
  // Not a checker: this proves the memory-location/sequencing schema is
  // extractable and deterministic. Sequencing semantics are the checker's
  // job (T4.x) and are not asserted here.
  // Uninstantiated template patterns are dependent, semantically-meaningless
  // expressions (DESIGN: analyze instantiations, not patterns) — skip them.
  // consteval bodies never execute at runtime either (T0.7) — but ordinary
  // constexpr functions still can, so those are NOT excluded here.
  // System-header bodies are deliberately included. Calls into the standard
  // library and compiler/runtime headers are part of the linked program's
  // behaviour; treating a system definition as an empty function merely
  // because its body facts were suppressed is an unsound under-approximation.
  // Keeping the origin on each emitted fact still permits policy/reporting
  // layers to distinguish the source when useful without sacrificing the
  // analysis itself.
  if (FD->doesThisDeclarationHaveABody() && !isTemplatePattern &&
      !FD->isConsteval()) {
    fullExprCounter_ = 0;
    nodeCounter_ = 0;
    localIds_.clear();
    escapedLocals_.clear();
    localEscapeFullExpr_.clear();
    localAccessFactIdx_.clear();
    unifyClassIds_.clear();
    returnClassIds_.clear();
    nextUnifyClassId_ = 0;
    pendingCallSiteReturnClassId_.clear();
    callResultClassIds_.clear();
    constructionReceivers_.clear();
    callSiteNodes_.clear();
    addrTakenTo_.clear();
    addrTakenAmbiguousPtrs_.clear();
    // `this` (implicit or explicit) is treated as a ParamRegion at a
    // RESERVED index one past the function's own real parameters — a
    // small, stable, collision-free slot (see paramRegionOf's CXXThisExpr
    // case and emitCallSite's receiver-arg append below), reusing all the
    // existing ParamRegion composition/remap machinery with no schema
    // change. FD->getNumParams() is 0 for a non-method free function,
    // harmless since CXXThisExpr can't appear in one anyway.
    currentFunctionParamCount_ = FD->getNumParams();
    currentFunctionDecl_ = FD;
    // Reset per function, alongside localIds_ (which starts from 0 too)
    // — offset into the upper half of the id space so a synthesized
    // temporary's Local id (see CXXBindTemporaryExpr below) can never
    // collide with a real local variable's id in the same function.
    nextTempLocalId_ = 0x80000000u;
    allocIdCounter_ = 0;
    allocationIds_.clear();
    // A constructor's member-initializer-list (base and member
    // initializers, user-written OR implicit) is stored separately from
    // getBody() and would otherwise never be visited (T0.6) — each
    // initializer's expression is its own full-expression.
    if (auto *CD = dyn_cast<CXXConstructorDecl>(FD)) {
      for (CXXCtorInitializer *Init : CD->inits()) {
        if (Expr *IE = Init->getInit()) {
          facts_pb::Aml thisObject;
          thisObject.mutable_param_region()->set_param_index(
              currentFunctionParamCount_);
          thisObject.mutable_param_region()->set_deref_depth(1);

          if (CXXConstructExpr *construction = directConstruction(IE)) {
            facts_pb::Aml receiver = thisObject;
            if (Init->isMemberInitializer()) {
              facts_pb::Field *field = receiver.mutable_field();
              *field->mutable_base() = thisObject;
              field->add_field_path(Init->getMember()->getFieldIndex());
            }
            constructionReceivers_[construction] = std::move(receiver);
          }

          // A pointer/reference member initializer copies a receiver value
          // into the constructed object just like an assignment in the
          // constructor body. Keep this as receiver-only value flow; the
          // analyzer remaps this->field and parameter sources through each
          // direct constructor call site.
          if (Init->isMemberInitializer()) {
            facts_pb::Aml destination;
            facts_pb::Field *field = destination.mutable_field();
            *field->mutable_base() = thisObject;
            field->add_field_path(Init->getMember()->getFieldIndex());
            emitReceiverTypeStore(destination, Init->getMember()->getType(),
                                  IE, fnId);
          }

          // A function-pointer member initialized by a constructor is a
          // concrete callback store just like `object.callback = target` in
          // the constructor body. Preserve the slot in the constructor's
          // `this` vocabulary; the analyzer remaps it through each constructor
          // call before binding a later member invocation's receiver.
          if (Init->isMemberInitializer() &&
              Init->getMember()->getType()->isFunctionPointerType()) {
            facts_pb::Aml rhs;
            bool resolved = resolvePointerTargetAml(IE, &rhs);
            bool isNull = !resolved &&
                IE->isNullPointerConstant(
                    Ctx, Expr::NPC_ValueDependentIsNotNull) !=
                    Expr::NPCK_NotNull;
            if (!isNull) {
              facts_pb::Fact storeFact;
              facts_pb::UnifyConstraint *uc =
                  storeFact.mutable_unify_constraint();
              *uc->mutable_function() = fnId;
              facts_pb::Field *field = uc->mutable_a()->mutable_field();
              *field->mutable_base() = thisObject;
              field->add_field_path(Init->getMember()->getFieldIndex());
              if (resolved) {
                *uc->mutable_b() = std::move(rhs);
              } else {
                uc->mutable_b()->mutable_widening()->set_kind(
                    facts_pb::Widening::W_UNKNOWN_EVERYTHING);
              }
              uc->set_reason(facts_pb::UnifyConstraint::R_FUNC_PTR_STORE);
              setLoc(uc->mutable_loc(), IE->getExprLoc(), SM);
              storeFact.set_origin(
                  originOf(isSystemLoc(IE->getExprLoc(), SM)));
              g_facts.push_back(std::move(storeFact));
            }
          }

          // A reference member is an alias, not a new object. Preserve that
          // binding so callback values forwarded through tuple/wrapper
          // constructors can reach later indirect calls on the same object.
          if (Init->isMemberInitializer() &&
              Init->getMember()->getType()->isReferenceType()) {
            facts_pb::Aml rhs;
            if (computeNamedVarAml(IE, &rhs) ||
                resolvePointerTargetAml(IE, &rhs)) {
              facts_pb::Fact aliasFact;
              facts_pb::UnifyConstraint *uc =
                  aliasFact.mutable_unify_constraint();
              *uc->mutable_function() = fnId;
              facts_pb::Field *field = uc->mutable_a()->mutable_field();
              *field->mutable_base() = thisObject;
              field->add_field_path(Init->getMember()->getFieldIndex());
              *uc->mutable_b() = std::move(rhs);
              uc->set_reason(facts_pb::UnifyConstraint::R_REFERENCE_ALIAS);
              setLoc(uc->mutable_loc(), IE->getExprLoc(), SM);
              aliasFact.set_origin(originOf(isSystemLoc(IE->getExprLoc(), SM)));
              g_facts.push_back(std::move(aliasFact));
            }
          }
          buildNode(IE, fnId, ++fullExprCounter_, /*writeCtx=*/false);
        }
      }
    }
    emitFullExprs(FD->getBody(), fnId);
    emitControlFlow(FD, fnId);
    currentFunctionDecl_ = nullptr;

    // Escape can be discovered anywhere in the function. Now that the whole
    // body has been scanned, mark only accesses in later full-expressions:
    // hidden global state cannot already point at a local first exposed by
    // the current expression, whose direct reachability is represented by
    // the call's ParamRegion effects. Indices into g_facts stay valid across
    // std::vector growth (we only ever append, never erase/reorder).
    for (const VarDecl *VD : escapedLocals_) {
      uint64_t escapeFullExpr = localEscapeFullExpr_.lookup(VD);
      for (size_t idx : localAccessFactIdx_[VD]) {
        facts_pb::Access *access = g_facts[idx].mutable_access();
        if (access->full_expr_id() <= escapeFullExpr) continue;
        if (facts_pb::Local *L = rootLocalOf(access->mutable_aml()))
          L->set_escaped(true);
      }
      // A local object that has its address taken or is reference-bound
      // IS potentially reachable from outside the function, so its
      // Local Aml is already marked escaped above (mayOverlap: escaped
      // local overlaps with everything).  But a tracked POINTER variable
      // p whose pointee is known (markReal'd to a concrete target) does
      // NOT get its pointee tainted here, even if p's own storage
      // address escaped.  Within a single full-expression — the unit
      // where sequencing checks run — p's value hasn't changed; a
      // sibling opaque call's effects are already modeled by its own
      // undefined-callee effects (W_UNKNOWN_GLOBAL, which correctly
      // overlaps with whatever concrete target p was assigned).
      // Tainting the pointee here would lose the proven disjointness
      // of a non-escaping local (mayOverlap returns false for it vs
      // W_UNKNOWN_GLOBAL/UnifyClass), creating false positives between
      // *p and an opaque-call sibling that has no args and so cannot
      // reach that local through any mechanism.
    }

    // Transitive escape closure: a local whose address was taken and
    // stored in a local pointer (p = &local) only escapes if that
    // pointer itself escapes. Un-escape locals whose address only
    // reached a non-escaping local pointer. Iterate to fixpoint for
    // chains (p = &local; q = &p; — if q doesn't escape, neither does
    // p, and neither does local).
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto &[localVD, ptrVD] : addrTakenTo_) {
        if (!escapedLocals_.count(ptrVD) &&
            !addrTakenAmbiguousPtrs_.count(ptrVD) &&
            escapedLocals_.count(localVD)) {
          escapedLocals_.erase(localVD);
          changed = true;
          // Un-escape the already-emitted Access facts for this local.
          for (size_t idx : localAccessFactIdx_[localVD])
            if (facts_pb::Local *L =
                    rootLocalOf(g_facts[idx].mutable_access()->mutable_aml()))
              L->set_escaped(false);
        }
      }
    }
  }
  return true;
}
