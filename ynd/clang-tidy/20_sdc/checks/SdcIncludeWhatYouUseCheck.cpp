#include "SdcIncludeWhatYouUseCheck.h"
#include "SdcIncludeWhatYouUseProviders.h"

#include "clang/AST/ComparisonCategories.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/FileEntry.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Format/Format.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/PPConditionalDirectiveRecord.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/Inclusions/HeaderIncludes.h"
#include "clang/Tooling/Inclusions/StandardLibrary.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/YAMLTraits.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace clang::tidy::sdc {
    struct SymbolProviderConfig {
        std::string Primary;
        std::vector<std::string> Secondary;
    };
} // namespace clang::tidy::sdc

namespace llvm::yaml {
    template <>
    struct MappingTraits<clang::tidy::sdc::SymbolProviderConfig> {
        static void mapping(IO& IO, clang::tidy::sdc::SymbolProviderConfig& Config) {
            IO.mapRequired("Primary", Config.Primary);
            IO.mapOptional("Secondary", Config.Secondary);
        }
    };
} // namespace llvm::yaml

LLVM_YAML_IS_STRING_MAP(std::vector<std::string>)
LLVM_YAML_IS_STRING_MAP(clang::tidy::sdc::SymbolProviderConfig)

namespace clang::tidy::sdc {
    namespace {

        using File = const FileEntry*;
        using Files = llvm::SmallVector<File, 4>;
        using Graph = llvm::DenseMap<File, Files>;
        using Bindings = llvm::SmallVector<std::pair<const NamedDecl*, bool>, 4>;
        using InstantiationDependencies = llvm::DenseMap<const NamedDecl*, Bindings>;
        using FacadeMap = std::map<std::string, std::vector<std::string>>;

        struct ProviderSet {
            Files Headers;
            // Keep the spelling even when this header is absent from the TU.
            std::string Primary;
        };

        struct Mention {
            // Preserve expansion context for generated declarations and
            // caller-supplied macro dependencies. Diagnostics use the spelling.
            SourceLocation Location;
            const NamedDecl* Binding;
            bool RequiresDefinition = false;
        };

        SourceLocation writtenLocation(const SourceManager& SM, SourceLocation Loc) {
            Loc = SM.getSpellingLoc(Loc);
            return Loc.isValid() && Loc.isFileID() && !SM.isInSystemHeader(Loc) &&
                           !SM.isWrittenInScratchSpace(Loc)
                       ? Loc
                       : SourceLocation();
        }

        bool sharesMacroExpansion(const SourceManager& SM, SourceLocation Left, SourceLocation Right) {
            if (!Left.isMacroID() || !Right.isMacroID()) {
                return false;
            }
            llvm::SmallVector<FileID, 8> Expansions;
            while (Left.isMacroID()) {
                Expansions.push_back(SM.getFileID(Left));
                Left = SM.getImmediateExpansionRange(Left).getBegin();
            }
            while (Right.isMacroID()) {
                if (llvm::is_contained(Expansions, SM.getFileID(Right))) {
                    return true;
                }
                Right = SM.getImmediateExpansionRange(Right).getBegin();
            }
            return false;
        }

        struct Include {
            File Header;
            std::string Spelling;
            std::string Path;
            File Parent;
            SourceLocation Location;
            CharSourceRange Range;
            bool Keep;
        };

        struct MacroReference {
            SourceLocation Location;
            File Definition;
        };

        void addUnique(Files& Values, File Value) {
            if (Value && !llvm::is_contained(Values, Value)) {
                Values.push_back(Value);
            }
        }

        Files reachable(File Start, const Graph& Edges) {
            Files Result;
            llvm::SmallPtrSet<File, 16> Seen;
            Files Pending{Start};
            while (!Pending.empty()) {
                File Current = Pending.pop_back_val();
                if (!Seen.insert(Current).second) {
                    continue;
                }
                Result.push_back(Current);
                auto Found = Edges.find(Current);
                if (Found != Edges.end()) {
                    Pending.append(Found->second.begin(), Found->second.end());
                }
            }
            return Result;
        }

        std::string normalizePath(StringRef Path) {
            if (Path.size() >= 2 &&
                ((Path.front() == '<' && Path.back() == '>') || (Path.front() == '"' && Path.back() == '"'))) {
                Path = Path.drop_front().drop_back();
            }
            llvm::SmallString<128> Result(Path);
            llvm::sys::path::remove_dots(Result, true);
            return Result.str().str();
        }

        // Find the written template binding without replacing aliases or selecting an
        // instantiated specialization. A using-declaration introduces its own binding.
        NamedDecl* templateBinding(TemplateName Name) {
            if (auto* Using = Name.getAsUsingShadowDecl()) {
                return Using;
            }
            auto* Decl = Name.getAsTemplateDecl();
            if (isa_and_nonnull<ClassTemplateDecl, TypeAliasTemplateDecl, TemplateTemplateParmDecl>(Decl)) {
                return Decl;
            }
            return nullptr;
        }

        const DeclContext* usingScope(const UsingDecl* Decl) {
            auto Qualifier = Decl->getQualifier();
#if LLVM_VERSION_MAJOR >= 22
            if (Qualifier.getKind() == NestedNameSpecifier::Kind::Namespace) {
                return Qualifier.getAsNamespaceAndPrefix().Namespace->getNamespace();
            }
            if (Qualifier.getKind() == NestedNameSpecifier::Kind::Global) {
                return Decl->getTranslationUnitDecl();
            }
            return Qualifier.getAsRecordDecl();
#else
            if (!Qualifier) {
                return nullptr;
            }
            if (auto* Namespace = Qualifier->getAsNamespace()) {
                return Namespace;
            }
            if (auto* Alias = Qualifier->getAsNamespaceAlias()) {
                return Alias->getNamespace();
            }
            if (Qualifier->getKind() == NestedNameSpecifier::Global) {
                return Decl->getTranslationUnitDecl();
            }
            if (auto* Type = Qualifier->getAsType()) {
                return Type->getAsCXXRecordDecl();
            }
            return Qualifier->getAsRecordDecl();
#endif
        }

        class MentionVisitor: public RecursiveASTVisitor<MentionVisitor> {
            using Base = RecursiveASTVisitor<MentionVisitor>;
            const SourceManager& SM;
            llvm::SmallVectorImpl<Mention>& Mentions;
            llvm::SmallVectorImpl<const NamedDecl*>* Declarations;
            std::set<std::pair<unsigned, const NamedDecl*>> Seen;

            // UsingShadowDecl stores the ultimate target, losing an intermediate using
            // binding. Recover that binding from the written qualifier's scope, while
            // keeping Clang's resolved target as the identity constraint.
            const NamedDecl* importedBinding(const DeclContext* Scope, const NamedDecl* Target, const UsingDecl* Use,
                                             llvm::SmallPtrSetImpl<const DeclContext*>& Visited) {
                if (!Scope) {
                    return nullptr;
                }
                if (const auto* Record = dyn_cast<CXXRecordDecl>(Scope)) {
                    if (const auto* Definition = Record->getDefinition()) {
                        Scope = Definition;
                    }
                }
                Scope = Scope->getPrimaryContext();
                if (!Visited.insert(Scope).second) {
                    return nullptr;
                }
                for (const auto* Candidate : Scope->lookup(Use->getDeclName())) {
                    if (!SM.isBeforeInTranslationUnit(Candidate->getLocation(), Use->getLocation())) {
                        continue;
                    }
                    if (Candidate->getUnderlyingDecl()->getCanonicalDecl() == Target->getCanonicalDecl()) {
                        return Candidate;
                    }
                }
                if (const auto* Namespace = dyn_cast<NamespaceDecl>(Scope)) {
                    for (const auto* Redeclaration : Namespace->redecls()) {
                        for (const auto* Decl : Redeclaration->decls()) {
                            const DeclContext* ImportedScope = nullptr;
                            if (const auto* Inline = dyn_cast<NamespaceDecl>(Decl); Inline && Inline->isInline()) {
                                ImportedScope = Inline;
                            } else if (const auto* Directive = dyn_cast<UsingDirectiveDecl>(Decl)) {
                                if (SM.isBeforeInTranslationUnit(Directive->getLocation(), Use->getLocation())) {
                                    ImportedScope = Directive->getNominatedNamespace();
                                }
                            }
                            if (const auto* Found = importedBinding(ImportedScope, Target, Use, Visited)) {
                                return Found;
                            }
                        }
                    }
                } else if (const auto* Record = dyn_cast<CXXRecordDecl>(Scope); Record && Record->hasDefinition()) {
                    for (const auto& Base : Record->bases()) {
                        if (const auto* Found =
                                importedBinding(Base.getType()->getAsCXXRecordDecl(), Target, Use, Visited)) {
                            return Found;
                        }
                    }
                }
                return nullptr;
            }

            void record(SourceLocation Loc, const NamedDecl* Binding) {
                if (!Binding || Loc.isInvalid() || isa<TemplateTypeParmDecl, TemplateTemplateParmDecl>(Binding)) {
                    return;
                }
                if (writtenLocation(SM, Loc).isInvalid()) {
                    return;
                }
                Binding = cast<NamedDecl>(Binding->getCanonicalDecl());
                if (Seen.emplace(Loc.getRawEncoding(), Binding).second) {
                    Mentions.push_back({Loc, Binding});
                }
            }

        public:
            MentionVisitor(const SourceManager& SM, llvm::SmallVectorImpl<Mention>& Mentions,
                           llvm::SmallVectorImpl<const NamedDecl*>* Declarations)
                : SM(SM)
                , Mentions(Mentions)
                , Declarations(Declarations)
            {
            }

            bool shouldVisitImplicitCode() const {
                return false;
            }
            bool shouldVisitTemplateInstantiations() const {
                return false;
            }
            bool shouldWalkTypesOfTypeLocs() const {
                return false;
            }
#if LLVM_VERSION_MAJOR >= 22
            bool TraverseType(QualType, bool = true) {
                return true;
            }
#else
            bool TraverseType(QualType) {
                return true;
            }
#endif

            bool VisitTagTypeLoc(TagTypeLoc Loc) {
                record(Loc.getNameLoc(), Loc.getDecl());
                return true;
            }

            bool VisitInjectedClassNameTypeLoc(InjectedClassNameTypeLoc Loc) {
                record(Loc.getNameLoc(), Loc.getDecl());
                return true;
            }

            bool VisitTypedefTypeLoc(TypedefTypeLoc Loc) {
                record(Loc.getNameLoc(), Loc.getTypePtr()->getDecl());
                return true;
            }

            bool VisitUsingTypeLoc(UsingTypeLoc Loc) {
#if LLVM_VERSION_MAJOR >= 22
                record(Loc.getNameLoc(), Loc.getDecl());
#else
                record(Loc.getNameLoc(), Loc.getFoundDecl());
#endif
                return true;
            }

            bool VisitTemplateSpecializationTypeLoc(TemplateSpecializationTypeLoc Loc) {
                record(Loc.getTemplateNameLoc(), templateBinding(Loc.getTypePtr()->getTemplateName()));
                return true;
            }

            bool VisitDeducedTemplateSpecializationTypeLoc(DeducedTemplateSpecializationTypeLoc Loc) {
                record(Loc.getTemplateNameLoc(), templateBinding(Loc.getTypePtr()->getTemplateName()));
                return true;
            }

            bool TraverseTemplateArgumentLoc(const TemplateArgumentLoc& Loc) {
                auto Kind = Loc.getArgument().getKind();
                if (Kind == TemplateArgument::Template || Kind == TemplateArgument::TemplateExpansion) {
                    record(Loc.getTemplateNameLoc(),
                           templateBinding(Loc.getArgument().getAsTemplateOrTemplatePattern()));
                }
                return Base::TraverseTemplateArgumentLoc(Loc);
            }

            bool VisitUsingDecl(UsingDecl* Decl) {
                if (SM.isInSystemHeader(SM.getSpellingLoc(Decl->getLocation()))) {
                    return true;
                }
                for (auto* Shadow : Decl->shadows()) {
                    auto* Target = Shadow->getTargetDecl();
                    if (isa<TypeDecl, ClassTemplateDecl, TypeAliasTemplateDecl>(Target)) {
                        llvm::SmallPtrSet<const DeclContext*, 8> Visited;
                        const auto* Imported = importedBinding(usingScope(Decl), Target, Decl, Visited);
                        record(Decl->getLocation(), Imported ? Imported : Target);
                    }
                }
                return true;
            }

            bool VisitNamedDecl(NamedDecl* Decl) {
                if (Declarations && Decl->getIdentifier()) {
                    Declarations->push_back(cast<NamedDecl>(Decl->getCanonicalDecl()));
                }
                return true;
            }

            bool TraverseVarTemplateSpecializationDecl(VarTemplateSpecializationDecl* Decl) {
                // Implicit variable specializations also appear directly in a
                // declaration context. RAV visits their synthesized argument
                // TypeLocs even with shouldVisitTemplateInstantiations() false.
                // Real uses are visited through the referring expression's
                // written template arguments instead.
                if (!isTemplateExplicitInstantiationOrSpecialization(Decl->getSpecializationKind())) {
                    return true;
                }
                return Base::TraverseVarTemplateSpecializationDecl(Decl);
            }

            bool VisitClassTemplateSpecializationDecl(ClassTemplateSpecializationDecl* Decl) {
                if (isTemplateExplicitInstantiationOrSpecialization(Decl->getSpecializationKind())) {
                    record(Decl->getLocation(), Decl->getSpecializedTemplate());
                }
                return true;
            }
        };

        // Unused includes need semantic dependencies as well as written names.
        // This traversal cannot produce missing-provider diagnostics.
        class ReferenceVisitor: public RecursiveASTVisitor<ReferenceVisitor> {
            using Base = RecursiveASTVisitor<ReferenceVisitor>;
            ASTContext& Context;
            const SourceManager& SM;
            llvm::SmallVectorImpl<Mention>& References;
            InstantiationDependencies& Dependencies;
            const NamedDecl* Instantiation = nullptr;
            const DeclContext* LookupContext = nullptr;
#if LLVM_VERSION_MAJOR < 22
            SourceLocation QualifiedTypeName;
            NestedNameSpecifierLoc TypeQualifier;
#endif
            ClassTemplateDecl* TupleSize = nullptr;
            ClassTemplateDecl* TupleElement = nullptr;
            bool LookedUpTupleTraits = false;

            const DeclContext* namespaceScope(NestedNameSpecifierLoc Loc) const {
                if (!Loc) {
                    return nullptr;
                }
                auto Qualifier = Loc.getNestedNameSpecifier();
#if LLVM_VERSION_MAJOR >= 22
                if (Qualifier.getKind() == NestedNameSpecifier::Kind::Namespace) {
                    return Qualifier.getAsNamespaceAndPrefix().Namespace->getNamespace();
                }
                if (Qualifier.getKind() == NestedNameSpecifier::Kind::Global) {
                    return Context.getTranslationUnitDecl();
                }
#else
                if (auto* Namespace = Qualifier->getAsNamespace()) {
                    return Namespace;
                }
                if (auto* Alias = Qualifier->getAsNamespaceAlias()) {
                    return Alias->getNamespace();
                }
                if (Qualifier->getKind() == NestedNameSpecifier::Global) {
                    return Context.getTranslationUnitDecl();
                }
#endif
                return nullptr;
            }

            bool recordNamespaceImports(SourceLocation Loc, const DeclContext* Scope, const DeclContext* Target,
                                        llvm::SmallPtrSetImpl<const DeclContext*>& Visited) {
                Scope = Scope->getPrimaryContext();
                for (const auto* Context = Target; Context;) {
                    if (Scope == Context->getPrimaryContext()) {
                        return true;
                    }
                    const auto* Namespace = dyn_cast<NamespaceDecl>(Context);
                    Context = Namespace && Namespace->isInline() ? Namespace->getParent() : nullptr;
                }
                if (!Visited.insert(Scope).second) {
                    return false;
                }
                bool Found = false;
                for (const auto* Directive : Scope->using_directives()) {
                    if (SM.isBeforeInTranslationUnit(Directive->getLocation(), Loc) &&
                        recordNamespaceImports(Loc, Directive->getNominatedNamespace(), Target, Visited)) {
                        if (!Directive->isImplicit()) {
                            recordReference(Loc, Directive);
                        }
                        Found = true;
                    }
                }
                // Another route may reach the same namespace through a different
                // header. Only guard cycles on the current path.
                Visited.erase(Scope);
                return Found;
            }

            void recordLookupImports(SourceLocation Loc, const NamedDecl* Binding,
                                     NestedNameSpecifierLoc Qualifier = {}) {
                if (!Binding || (!Instantiation && writtenLocation(SM, Loc).isInvalid())) {
                    return;
                }
                const auto* Target = Binding->getDeclContext()->getRedeclContext();
                if (!Target->isFileContext()) {
                    return;
                }
                const auto* Scope = Qualifier ? namespaceScope(Qualifier) : LookupContext;
                for (; Scope; Scope = Qualifier ? nullptr : Scope->getParent()) {
                    if (Scope->isFileContext()) {
                        llvm::SmallPtrSet<const DeclContext*, 8> Visited;
                        if (recordNamespaceImports(Loc, Scope, Target, Visited)) {
                            return;
                        }
                    }
                }
            }

            void findTupleTraits(const NamespaceDecl* Namespace,
                                 llvm::SmallPtrSetImpl<const NamespaceDecl*>& Visited) {
                if (!Visited.insert(Namespace->getCanonicalDecl()).second) {
                    return;
                }
                for (auto [Name, Template] : {std::make_pair("tuple_size", &TupleSize),
                                              std::make_pair("tuple_element", &TupleElement)}) {
                    for (auto* Decl : Namespace->lookup(&Context.Idents.get(Name))) {
                        if (auto* Found = dyn_cast<ClassTemplateDecl>(Decl->getUnderlyingDecl())) {
                            *Template = Found;
                        }
                    }
                }
                for (const auto* Redeclaration : Namespace->redecls()) {
                    for (const auto* Decl : Redeclaration->decls()) {
                        if (const auto* Inline = dyn_cast<NamespaceDecl>(Decl); Inline && Inline->isInline()) {
                            findTupleTraits(Inline, Visited);
                        }
                    }
                }
            }

            void recordTupleTrait(SourceLocation Loc, ClassTemplateDecl* Template,
                                  ArrayRef<TemplateArgument> Arguments) {
                if (!Template) {
                    return;
                }
                void* InsertPosition = nullptr;
                // Sema already instantiated the selected protocol. Never form
                // new specializations while analyzing include dependencies.
                if (auto* Specialization = Template->findSpecialization(Arguments, InsertPosition)) {
                    recordReference(Loc, Specialization, true);
                }
            }

            void recordReference(SourceLocation Loc, const NamedDecl* Binding, bool RequiresDefinition = false) {
                if (!Binding) {
                    return;
                }
                Binding = cast<NamedDecl>(Binding->getCanonicalDecl());
                if (Instantiation) {
                    // Attribute these dependencies to each consumer of the
                    // instantiation, not to the template's spelling location or
                    // only its first point of instantiation. This also handles
                    // user customization points called from system templates.
                    auto& Used = Dependencies[Instantiation];
                    auto Dependency = std::make_pair(Binding, RequiresDefinition);
                    if (Binding != Instantiation && !llvm::is_contained(Used, Dependency)) {
                        Used.push_back(Dependency);
                    }
                    return;
                }
                if (writtenLocation(SM, Loc).isValid()) {
                    References.push_back({Loc, Binding, RequiresDefinition});
                }
            }

            void recordTypeReference(SourceLocation Loc, QualType Type, bool RequiresDefinition = false) {
                if (Type.isNull()) {
                    return;
                }
                if (RequiresDefinition) {
                    Type = Context.getBaseElementType(Type);
                }
                while (Type->isPointerType() || Type->isReferenceType()) {
                    RequiresDefinition = false;
                    Type = Type->getPointeeType();
                }
                if (const auto* Decl = Type->getAsTagDecl()) {
                    recordReference(Loc, Decl, RequiresDefinition);
                }
            }

            void recordPlacementCandidates(const CXXNewExpr* Expr) {
                auto Name = Context.DeclarationNames.getCXXOperatorName(Expr->isArray() ? OO_Array_New : OO_New);
                for (const auto* Candidate : Context.getTranslationUnitDecl()->lookup(Name)) {
                    const auto* Function = dyn_cast<FunctionDecl>(Candidate);
                    if (const auto* Template = dyn_cast<FunctionTemplateDecl>(Candidate)) {
                        Function = Template->getTemplatedDecl();
                    }
                    if (!Function || (Function->getNumParams() < 2 && !Function->isVariadic())) {
                        continue;
                    }
                    for (const auto* Declaration : Function->redecls()) {
                        if (!Declaration->isImplicit() && Declaration->getLocation().isValid() &&
                            SM.isBeforeInTranslationUnit(Declaration->getLocation(), Expr->getExprLoc())) {
                            recordReference(Expr->getExprLoc(), Declaration);
                            break;
                        }
                    }
                }
            }

        public:
            ReferenceVisitor(ASTContext& Context, llvm::SmallVectorImpl<Mention>& References,
                             InstantiationDependencies& Dependencies)
                : Context(Context)
                , SM(Context.getSourceManager())
                , References(References)
                , Dependencies(Dependencies)
            {
            }

            bool shouldVisitTemplateInstantiations() const {
                return true;
            }
            bool shouldWalkTypesOfTypeLocs() const {
                return false;
            }
#if LLVM_VERSION_MAJOR >= 22
            bool TraverseType(QualType, bool = true) {
                return true;
            }
#else
            bool TraverseType(QualType) {
                return true;
            }
#endif

            bool TraverseDecl(Decl* Decl) {
                if (!Decl) {
                    return true;
                }
                auto Kind = TSK_Undeclared;
                SourceLocation Point;
                if (const auto* Function = dyn_cast<FunctionDecl>(Decl)) {
                    Kind = Function->getTemplateSpecializationKind();
                    Point = Function->getPointOfInstantiation();
                } else if (const auto* Record = dyn_cast<ClassTemplateSpecializationDecl>(Decl)) {
                    Kind = Record->getSpecializationKind();
                    Point = Record->getPointOfInstantiation();
                } else if (const auto* Variable = dyn_cast<VarDecl>(Decl)) {
                    Kind = Variable->getTemplateSpecializationKind();
                    Point = Variable->getPointOfInstantiation();
                }
                const auto* Previous = Instantiation;
                if (isTemplateInstantiation(Kind)) {
                    auto* Binding = cast<NamedDecl>(Decl->getCanonicalDecl());
                    if (Kind == TSK_ExplicitInstantiationDeclaration || Kind == TSK_ExplicitInstantiationDefinition) {
                        // An explicit instantiation consumes its dependencies
                        // even if there is no call or object expression.
                        recordReference(Point, Binding);
                    }
                    Instantiation = Binding;
                }
                const auto* PreviousContext = LookupContext;
                LookupContext = dyn_cast<DeclContext>(Decl);
                if (!LookupContext) {
                    LookupContext = Decl->getDeclContext();
                }
                bool Result = Base::TraverseDecl(Decl);
                LookupContext = PreviousContext;
                Instantiation = Previous;
                return Result;
            }

            bool VisitCXXRecordDecl(CXXRecordDecl* Decl) {
                const auto* Specialization = dyn_cast<ClassTemplateSpecializationDecl>(Decl);
                if (!Specialization || !isTemplateInstantiation(Specialization->getSpecializationKind()) ||
                    !Decl->isThisDeclarationADefinition()) {
                    return true;
                }
                // Constructing a polymorphic specialization can instantiate its
                // virtual callbacks without a direct call to the overriding method.
                // Only follow bodies Sema actually instantiated, not unused hooks.
                for (const auto* Method : Decl->methods()) {
                    if (Method->isVirtual() && Method->doesThisDeclarationHaveABody()) {
                        recordReference(Decl->getLocation(), Method);
                    }
                }
                return true;
            }

            bool VisitVarTemplateSpecializationDecl(VarTemplateSpecializationDecl* Decl) {
                if (isTemplateExplicitInstantiationOrSpecialization(Decl->getSpecializationKind())) {
                    recordReference(Decl->getLocation(), Decl->getSpecializedTemplate());
                }
                return true;
            }

            template <class NamedTypeLoc>
            void recordTypeLookup(NamedTypeLoc Loc, const NamedDecl* Binding) {
#if LLVM_VERSION_MAJOR >= 22
                recordLookupImports(Loc.getBeginLoc(), Binding, Loc.getQualifierLoc());
#else
                recordLookupImports(Loc.getBeginLoc(), Binding,
                                    Loc.getBeginLoc() == QualifiedTypeName ? TypeQualifier : NestedNameSpecifierLoc());
#endif
            }

            bool VisitTypeLoc(TypeLoc Loc) {
                // A type-only operand, e.g. sizeof(Box<int>), consumes the
                // selected specialization even though the expression is integer.
                recordTypeReference(Loc.getBeginLoc(), Loc.getType());
                if (auto Tag = Loc.getAs<TagTypeLoc>()) {
                    recordTypeLookup(Tag, Tag.getDecl());
                } else if (auto Alias = Loc.getAs<TypedefTypeLoc>()) {
                    recordTypeLookup(Alias, Alias.getTypePtr()->getDecl());
                } else if (auto Using = Loc.getAs<UsingTypeLoc>()) {
#if LLVM_VERSION_MAJOR >= 22
                    recordTypeLookup(Using, Using.getDecl());
#else
                    recordTypeLookup(Using, Using.getFoundDecl());
#endif
                } else if (auto Template = Loc.getAs<TemplateSpecializationTypeLoc>()) {
                    recordTypeLookup(Template, templateBinding(Template.getTypePtr()->getTemplateName()));
                } else if (auto Deduced = Loc.getAs<DeducedTemplateSpecializationTypeLoc>()) {
                    recordTypeLookup(Deduced, templateBinding(Deduced.getTypePtr()->getTemplateName()));
                }
                return true;
            }

#if LLVM_VERSION_MAJOR < 22
            bool TraverseElaboratedTypeLoc(ElaboratedTypeLoc Loc) {
                auto PreviousName = QualifiedTypeName;
                auto PreviousQualifier = TypeQualifier;
                QualifiedTypeName = Loc.getNamedTypeLoc().getBeginLoc();
                TypeQualifier = Loc.getQualifierLoc();
                bool Result = Base::TraverseElaboratedTypeLoc(Loc);
                QualifiedTypeName = PreviousName;
                TypeQualifier = PreviousQualifier;
                return Result;
            }
#endif

            bool VisitUsingDecl(UsingDecl* Decl) {
                for (auto* Shadow : Decl->shadows()) {
                    recordReference(Decl->getLocation(), Shadow->getTargetDecl());
                }
                return true;
            }

            bool VisitDeclRefExpr(DeclRefExpr* Expr) {
                recordReference(Expr->getLocation(), Expr->getFoundDecl());
                recordReference(Expr->getLocation(), Expr->getDecl());
                recordLookupImports(Expr->getLocation(), Expr->getFoundDecl(), Expr->getQualifierLoc());
                return true;
            }

            bool VisitMemberExpr(MemberExpr* Expr) {
                recordReference(Expr->getMemberLoc(), Expr->getFoundDecl().getDecl());
                recordReference(Expr->getMemberLoc(), Expr->getMemberDecl());
                return true;
            }

            bool VisitOverloadExpr(OverloadExpr* Expr) {
                for (const auto* Decl : Expr->decls()) {
                    recordReference(Expr->getNameLoc(), Decl);
                    recordLookupImports(Expr->getNameLoc(), Decl, Expr->getQualifierLoc());
                }
                return true;
            }

            bool VisitCXXConstructExpr(CXXConstructExpr* Expr) {
                recordReference(Expr->getLocation(), Expr->getConstructor());
                return true;
            }

            bool VisitCXXNewExpr(CXXNewExpr* Expr) {
                recordReference(Expr->getExprLoc(), Expr->getOperatorNew());
                recordReference(Expr->getExprLoc(), Expr->getOperatorDelete());
                if (!Expr->getOperatorNew() && Expr->getNumPlacementArgs()) {
                    // Dependent placement new has no selected function yet. Keep
                    // visible global candidates at the template's written use,
                    // even when no specialization is instantiated in this TU.
                    recordPlacementCandidates(Expr);
                }
                return true;
            }

            bool TraverseCXXRewrittenBinaryOperator(CXXRewrittenBinaryOperator* Expr) {
                // The semantic form retains the selected == or <=> function,
                // including a range-for condition rewritten from !=.
                return WalkUpFromCXXRewrittenBinaryOperator(Expr) && TraverseStmt(Expr->getSemanticForm());
            }

            bool VisitCXXForRangeStmt(CXXForRangeStmt* Statement) {
                // RAV omits these synthesized operations unless implicit-code
                // traversal is enabled. Visit just the range-for dependencies:
                // begin/end, iterator comparison/increment and dereference.
                for (auto* Init : {Statement->getBeginStmt(), Statement->getEndStmt()}) {
                    if (Init) {
                        auto* Variable = cast<VarDecl>(Init->getSingleDecl());
                        if (!VisitVarDecl(Variable) || !TraverseStmt(Variable->getInit())) {
                            return false;
                        }
                    }
                }
                auto* Variable = Statement->getLoopVariable();
                return TraverseStmt(Statement->getCond()) && TraverseStmt(Statement->getInc()) &&
                       (!Variable || TraverseStmt(Variable->getInit()));
            }

            bool VisitDecompositionDecl(DecompositionDecl* Decl) {
                auto Bindings = Decl->bindings();
                if (Bindings.empty() || !Bindings.front()->getHoldingVar()) {
                    return true;
                }
                if (!LookedUpTupleTraits) {
                    LookedUpTupleTraits = true;
                    llvm::SmallPtrSet<const NamespaceDecl*, 8> Visited;
                    for (const auto* Decl : Context.getTranslationUnitDecl()->lookup(&Context.Idents.get("std"))) {
                        if (const auto* Namespace = dyn_cast<NamespaceDecl>(Decl)) {
                            findTupleTraits(Namespace, Visited);
                        }
                    }
                }
                auto Type = Context.getCanonicalType(Decl->getType().getNonReferenceType());
                recordTupleTrait(Decl->getLocation(), TupleSize, {TemplateArgument(Type)});
                for (unsigned Index = 0; Index < Bindings.size(); ++Index) {
                    auto IndexType = Context.getSizeType();
                    recordTupleTrait(Bindings[Index]->getLocation(), TupleElement,
                                     {TemplateArgument(Context, Context.MakeIntValue(Index, IndexType), IndexType),
                                      TemplateArgument(Type)});
                }
                return true;
            }

            bool VisitBindingDecl(BindingDecl* Decl) {
                // RAV skips the implicit get call and binding expression by
                // default. Keep them in the unused-include dependency visitor.
                if (auto* Variable = Decl->getHoldingVar()) {
                    if (!VisitVarDecl(Variable) || !TraverseStmt(Variable->getInit())) {
                        return false;
                    }
                }
                return TraverseStmt(Decl->getBinding());
            }

            bool VisitCXXBindTemporaryExpr(CXXBindTemporaryExpr* Expr) {
                recordReference(Expr->getExprLoc(), Expr->getTemporary()->getDestructor());
                return true;
            }

            bool VisitConceptReference(ConceptReference* Reference) {
                // Constrained parameters and auto declarations need their
                // concept headers even without a ConceptSpecializationExpr.
                recordReference(Reference->getConceptNameLoc(), Reference->getFoundDecl());
                recordReference(Reference->getConceptNameLoc(), Reference->getNamedConcept());
                recordLookupImports(Reference->getConceptNameLoc(), Reference->getFoundDecl(),
                                    Reference->getNestedNameSpecifierLoc());
                return true;
            }

            bool TraverseNestedNameSpecifierLoc(NestedNameSpecifierLoc Loc) {
                if (Loc) {
#if LLVM_VERSION_MAJOR >= 22
                    auto Qualifier = Loc.getNestedNameSpecifier();
                    if (Qualifier.getKind() == NestedNameSpecifier::Kind::Namespace) {
                        const auto [Namespace, Prefix] = Loc.getAsNamespaceAndPrefix();
                        recordReference(Loc.getLocalBeginLoc(), dyn_cast<NamespaceAliasDecl>(Namespace));
                        recordLookupImports(Loc.getLocalBeginLoc(), Namespace, Prefix);
                    }
#else
                    const auto* Qualifier = Loc.getNestedNameSpecifier();
                    const auto* Alias = Qualifier->getAsNamespaceAlias();
                    recordReference(Loc.getLocalBeginLoc(), Alias);
                    recordLookupImports(Loc.getLocalBeginLoc(),
                                        Alias ? static_cast<const NamedDecl*>(Alias) : Qualifier->getAsNamespace(),
                                        Loc.getPrefix());
#endif
                }
                // The base traversal visits prefixes too: alias::nested::Type
                // consumes the alias declaration as well as the final type.
                return Base::TraverseNestedNameSpecifierLoc(Loc);
            }

            bool VisitUsingDirectiveDecl(UsingDirectiveDecl* Decl) {
                recordReference(Decl->getLocation(), Decl->getNominatedNamespaceAsWritten());
                recordLookupImports(Decl->getLocation(), Decl->getNominatedNamespaceAsWritten(), Decl->getQualifierLoc());
                return true;
            }

            bool VisitNamespaceAliasDecl(NamespaceAliasDecl* Decl) {
                // Preserve an intermediate alias binding rather than replacing
                // it with the ultimate namespace, which may have another header.
                recordReference(Decl->getTargetNameLoc(), Decl->getAliasedNamespace());
                recordLookupImports(Decl->getTargetNameLoc(), Decl->getAliasedNamespace(), Decl->getQualifierLoc());
                return true;
            }

            bool VisitNamedDecl(NamedDecl* Decl) {
                // Definitions consume earlier declarations. Reopening a
                // namespace does not consume every header declaring it.
                if (isa<FunctionDecl, VarDecl, TagDecl>(Decl)) {
                    recordReference(Decl->getLocation(), Decl);
                }
                return true;
            }

            bool VisitExpr(Expr* Expr) {
                recordTypeReference(Expr->getExprLoc(), Expr->getType());
                return true;
            }

            bool VisitFunctionDecl(FunctionDecl* Decl) {
                if (!Decl->isDefaulted() || Decl->getOverloadedOperator() != OO_Spaceship) {
                    return true;
                }
                // A defaulted comparison can require a category without a
                // written return type or a body for the visitor to traverse.
                auto ReturnType = Decl->getReturnType();
                recordTypeReference(Decl->getLocation(), ReturnType);
                if (ReturnType->isDependentType() || ReturnType->isUndeducedType()) {
                    // An uninstantiated template can select any category. Keep
                    // its visible providers without adding written-type uses.
                    for (auto Kind : {ComparisonCategoryType::StrongOrdering,
                                      ComparisonCategoryType::WeakOrdering,
                                      ComparisonCategoryType::PartialOrdering}) {
                        if (const auto* Category = Context.CompCategories.lookupInfo(Kind)) {
                            recordReference(Decl->getLocation(), Category->Record);
                        }
                    }
                }
                return true;
            }

            bool VisitUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr* Expr) {
                recordTypeReference(Expr->getExprLoc(), Expr->getTypeOfArgument(), true);
                return true;
            }

            bool VisitFieldDecl(FieldDecl* Decl) {
                recordTypeReference(Decl->getLocation(), Decl->getType(), true);
                return true;
            }

            bool VisitVarDecl(VarDecl* Decl) {
                auto Location = Decl->getLocation();
                if (const auto* Init = Decl->getInit()) {
                    if (Init->getExprLoc().isMacroID()) {
                        // An inferred result of a macro does not make its
                        // implementation types caller-owned dependencies.
                        Location = Init->getExprLoc();
                    }
                }
                bool RequiresDefinition = Decl->isThisDeclarationADefinition() != VarDecl::DeclarationOnly;
                if (isa<ParmVarDecl>(Decl)) {
                    const auto* Function = dyn_cast<FunctionDecl>(Decl->getDeclContext());
                    RequiresDefinition = Function && Function->doesThisDeclarationHaveABody();
                }
                recordTypeReference(Location, Decl->getType(), RequiresDefinition);
                return true;
            }
        };

    } // namespace

    class SdcIncludeWhatYouUseCheck::Analysis: public CommentHandler {
    public:
        Analysis(SdcIncludeWhatYouUseCheck& Check, ClangTidyContext& TidyContext, StringRef Facades,
                 StringRef SymbolProviders, bool CheckUnusedIncludes)
            : Check(Check), TidyContext(TidyContext), Facades(Facades.str()), SymbolProviders(SymbolProviders.str()),
              CheckUnusedIncludes(CheckUnusedIncludes) {}

        SdcIncludeWhatYouUseCheck& Check;
        ClangTidyContext& TidyContext;
        std::string Facades;
        std::string SymbolProviders;
        std::map<std::string, ProviderSet> ConfiguredProviders;
        bool CheckUnusedIncludes;
        const SourceManager* SM = nullptr;
        ASTContext* Context = nullptr;
        Preprocessor* PP = nullptr;
        llvm::SmallVector<Include, 64> Includes;
        Graph DirectIncludes;
        Graph Exporters;
        llvm::DenseMap<FileID, unsigned> ExportBlocks;
        Graph UsedIncludes;
        llvm::SmallPtrSet<File, 32> DiagnosticEffectHeaders;
        llvm::SmallPtrSet<File, 32> MacroCleanupHeaders;
        llvm::StringMap<Files> PublicHeaders;
        llvm::DenseMap<File, llvm::SmallPtrSet<File, 32>> ReachableCache;
        llvm::DenseSet<std::pair<File, File>> UsedIncludePaths;
        llvm::SmallVector<MacroReference, 32> MacroReferences;
        llvm::SmallPtrSet<const MacroInfo*, 32> UnobservedMacros;
        PPConditionalDirectiveRecord* Conditions = nullptr;
        NoLintDirectiveHandler UseSuppressions;
        llvm::SmallPtrSet<File, 16> ManualIncludePlacement;
        llvm::DenseMap<File, SourceLocation> FirstInclude;
        llvm::DenseMap<FileID, llvm::SmallVector<unsigned, 4>> PlacementBarriers;
        llvm::DenseMap<const NamedDecl*, ProviderSet> ProviderCache;
        llvm::DenseMap<const NamedDecl*, Files> DefinitionProviderCache;
        llvm::DenseMap<const NamedDecl*, Files> DeclarationCache;
        tooling::stdlib::Recognizer StandardLibrary;
        bool InvalidConfiguration = false;

        File fileAt(SourceLocation Loc) const {
            return Loc.isValid() ? SM->getFileEntryForID(SM->getFileID(Loc)) : nullptr;
        }

        bool hasPragma(CharSourceRange Range, StringRef Pragma) const {
            auto End = Range.getEnd();
            bool TokenRange = Range.isTokenRange();
            if (End.isMacroID()) {
                End = SM->getExpansionRange(End).getEnd();
                TokenRange = true;
            }
            if (TokenRange) {
                End = Lexer::getLocForEndOfToken(End, 0, *SM, PP->getLangOpts());
            }
            if (End.isInvalid() || !End.isFileID()) {
                return false;
            }
            bool Invalid = false;
            auto Buffer = SM->getBufferData(SM->getFileID(End), &Invalid);
            if (Invalid) {
                return false;
            }
            StringRef Tail = Buffer.drop_front(SM->getFileOffset(End)).split('\n').first.trim();
            if (Tail.consume_front("//")) {
                return Tail.trim() == Pragma;
            }
            if (Tail.consume_front("/*") && Tail.consume_back("*/")) {
                return Tail.trim() == Pragma;
            }
            return false;
        }

        bool HandleComment(Preprocessor &, SourceRange Range) override {
          StringRef Text = Lexer::getSourceText(
              CharSourceRange::getCharRange(Range), *SM, PP->getLangOpts());
          if (Text.consume_front("//")) {
            Text = Text.trim();
          } else if (Text.consume_front("/*") && Text.consume_back("*/")) {
            Text = Text.trim();
          } else {
            return false;
          }
          // An export block belongs to one physical inclusion. A block in a
          // parent cannot export a child's dependencies or leak into siblings.
          auto FID = SM->getFileID(Range.getBegin());
          if (Text == "IWYU pragma: begin_exports") {
            ++ExportBlocks[FID];
            ManualIncludePlacement.insert(fileAt(Range.getBegin()));
          } else if (Text == "IWYU pragma: end_exports") {
            auto Found = ExportBlocks.find(FID);
            if (Found != ExportBlocks.end() && --Found->second == 0) {
              ExportBlocks.erase(Found);
            }
          }
          return false;
        }

        class IncludeRecorder: public PPCallbacks {
            Analysis& State;

            // Persistent stack frames let us compare the state on file entry
            // and exit without copying the diagnostic stack at every include.
            // A balanced push/pop is local; an escaping change is a dependency.
            using DiagnosticState = std::pair<unsigned, unsigned>;
            DiagnosticState Diagnostics = {0, 0};
            unsigned NextDiagnosticMapping = 0;
            llvm::SmallVector<DiagnosticState, 32> DiagnosticStack;
            llvm::DenseMap<FileID, DiagnosticState> EntryDiagnostics;
            // Record a macro's initial state only when it changes. Propagating
            // first changes at file exit also accounts for nested cleanup headers.
            using MacroStates = llvm::DenseMap<const IdentifierInfo*, bool>;
            llvm::DenseMap<FileID, MacroStates> EntryMacros;
            llvm::DenseMap<const MacroInfo*, Files> MacroProviders;

            const Files& macroProviders(const MacroDefinition& Definition) {
                const auto* Macro = Definition.getMacroInfo();
                auto Found = MacroProviders.find(Macro);
                if (Found != MacroProviders.end()) {
                    return Found->second;
                }
                Files Headers;
                const auto& SM = *State.SM;
                addUnique(Headers, State.fileAt(SM.getExpansionLoc(Macro->getDefinitionLoc())));
                // libc headers can independently define the same public macro.
                // Credit equivalent definitions already visible at this use;
                // an undef or a changed replacement list ends that history.
                for (const MacroDirective* Directive = Definition.getLocalDirective(); Directive;
                     Directive = Directive->getPrevious()) {
                    if (isa<UndefMacroDirective>(Directive)) {
                        break;
                    }
                    if (const auto* Def = dyn_cast<DefMacroDirective>(Directive)) {
                        const auto* Previous = Def->getInfo();
                        if (Previous != Macro && !Macro->isIdenticalTo(*Previous, *State.PP, false)) {
                            break;
                        }
                        addUnique(Headers, State.fileAt(SM.getExpansionLoc(Previous->getDefinitionLoc())));
                    }
                }
                return MacroProviders.try_emplace(Macro, std::move(Headers)).first->second;
            }

            void recordMacroChange(const Token& Token, bool WasDefined) {
                auto FID = State.SM->getFileID(State.SM->getSpellingLoc(Token.getLocation()));
                auto Entry = EntryMacros.find(FID);
                if (Entry != EntryMacros.end()) {
                    Entry->second.try_emplace(Token.getIdentifierInfo(), WasDefined);
                }
            }

            void recordPlacementBarrier(SourceLocation Location) {
                auto Loc = writtenLocation(*State.SM, Location);
                if (Loc.isValid()) {
                    auto [FID, Offset] = State.SM->getDecomposedLoc(Loc);
                    auto& Barriers = State.PlacementBarriers[FID];
                    // A pragma in an include filename expands before InclusionDirective.
                    if (!Barriers.empty() && Offset < Barriers.back()) {
                        Barriers.insert(llvm::lower_bound(Barriers, Offset), Offset);
                    } else {
                        Barriers.push_back(Offset);
                    }
                }
            }

            void finishMacroChanges(FileID FID, FileID ParentFID) {
                auto Entry = EntryMacros.find(FID);
                if (Entry == EntryMacros.end()) {
                    return;
                }
                auto Parent = EntryMacros.find(ParentFID);
                for (const auto& [Name, WasDefined] : Entry->second) {
                    if (WasDefined && !State.PP->getMacroDefinition(Name).getMacroInfo()) {
                        if (File Header = State.SM->getFileEntryForID(FID)) {
                            State.MacroCleanupHeaders.insert(Header);
                        }
                    }
                    if (Parent != EntryMacros.end()) {
                        Parent->second.try_emplace(Name, WasDefined);
                    }
                }
                EntryMacros.erase(Entry);
            }

            void recordMacro(const Token& Token, const MacroDefinition& Definition) {
                if (!State.CheckUnusedIncludes || !Definition.getMacroInfo()) {
                    return;
                }
                const auto& SM = *State.SM;
                auto Location = writtenLocation(SM, Token.getLocation());
                if (Location.isValid()) {
                    for (File Header : macroProviders(Definition)) {
                        State.MacroReferences.push_back({Location, Header});
                    }
                }
            }

        public:
            explicit IncludeRecorder(Analysis& State)
                : State(State)
            {
            }

            void LexedFileChanged(FileID FID, LexedFileChangeReason Reason, SrcMgr::CharacteristicKind,
                                  FileID PrevFID, SourceLocation) override {
                if (!State.CheckUnusedIncludes) {
                    return;
                }
                if (Reason == LexedFileChangeReason::EnterFile) {
                    EntryDiagnostics[FID] = Diagnostics;
                    EntryMacros.try_emplace(FID);
                    return;
                }
                finishMacroChanges(PrevFID, FID);
                auto Entry = EntryDiagnostics.find(PrevFID);
                if (Entry == EntryDiagnostics.end()) {
                    return;
                }
                if (Entry->second != Diagnostics) {
                    if (File Header = State.SM->getFileEntryForID(PrevFID)) {
                        State.DiagnosticEffectHeaders.insert(Header);
                    }
                }
                EntryDiagnostics.erase(Entry);
            }

            void PragmaDirective(SourceLocation Location, PragmaIntroducerKind) override {
                // A macro can expand to _Pragma and change state in the caller.
                recordPlacementBarrier(State.SM->getExpansionLoc(Location));
            }

            void PragmaDiagnosticPush(SourceLocation, StringRef) override {
                if (State.CheckUnusedIncludes) {
                    DiagnosticStack.push_back(Diagnostics);
                    Diagnostics.second = DiagnosticStack.size();
                }
            }

            void PragmaDiagnosticPop(SourceLocation, StringRef) override {
                if (State.CheckUnusedIncludes && Diagnostics.second) {
                    Diagnostics = DiagnosticStack[Diagnostics.second - 1];
                }
            }

            void PragmaDiagnostic(SourceLocation, StringRef, diag::Severity, StringRef) override {
                if (State.CheckUnusedIncludes) {
                    Diagnostics.first = ++NextDiagnosticMapping;
                }
            }

            void InclusionDirective(SourceLocation HashLoc, const Token&, StringRef Spelling, bool Angled,
                                    CharSourceRange Range, OptionalFileEntryRef Header, StringRef, StringRef,
                                    const Module*, bool, SrcMgr::CharacteristicKind) override {
                // A direct include can configure later headers through any depth
                // of inclusion, even when its body is skipped in this TU.
                recordPlacementBarrier(HashLoc);
                File Parent = State.fileAt(HashLoc);
                if (!Parent || !Header) {
                    return;
                }
                File Included = &Header->getFileEntry();
                State.FirstInclude.try_emplace(Parent, HashLoc);
                auto Leaf = llvm::sys::path::filename(Spelling);
                if (Leaf == "webrtc_workaround_start.h" || Leaf == "webrtc_workaround_end.h") {
                    State.ManualIncludePlacement.insert(Parent);
                }
                bool Export =
                    State.ExportBlocks.count(State.SM->getFileID(HashLoc)) ||
                    State.hasPragma(Range, "IWYU pragma: export");
                bool Keep = Export || State.hasPragma(Range, "IWYU pragma: keep");
                State.Includes.push_back(
                    {Included, Spelling.str(), Header->getName().str(), Parent, HashLoc, Range, Keep});
                addUnique(State.DirectIncludes[Parent], Included);
                if (Angled) {
                    addUnique(State.PublicHeaders[normalizePath(Spelling)], Included);
                }
                if (Export) {
                    addUnique(State.Exporters[Included], Parent);
                }
            }

            void MacroDefined(const Token& Token, const MacroDirective* Directive) override {
                recordPlacementBarrier(Token.getLocation());
                if (!State.CheckUnusedIncludes) {
                    return;
                }
                const auto* Previous = Directive->getPrevious();
                recordMacroChange(Token, Previous && Previous->isDefined());
                const auto* Macro = Directive->getMacroInfo();
                const auto Location = writtenLocation(*State.SM, Macro->getDefinitionLoc());
                if (Location.isInvalid() || State.SM->isWrittenInMainFile(Location)) {
                    return;
                }
                if (Macro->tokens_empty()) {
                    return;
                }
                if (Macro->getNumTokens() == 1) {
                    const auto& Token = Macro->getReplacementToken(0);
                    if (Token.isOneOf(tok::kw_true, tok::kw_false, tok::kw_nullptr) ||
                        (Token.is(tok::identifier) && Macro->getParameterNum(Token.getIdentifierInfo()) >= 0)) {
                        return;
                    }
                    if (Token.is(tok::numeric_constant)) {
                        auto Spelling = Lexer::getSpelling(Token, *State.SM, State.PP->getLangOpts());
                        if (llvm::all_of(Spelling, [](char C) { return C >= '0' && C <= '9'; })) {
                            return;
                        }
                    }
                }
                // Other replacement lists may hide semantic dependencies even
                // without external identifiers, e.g. placement new or a UDL.
                State.UnobservedMacros.insert(Macro);
            }

            void MacroUndefined(const Token& Token, const MacroDefinition& Definition,
                                const MacroDirective*) override {
                recordPlacementBarrier(Token.getLocation());
                if (State.CheckUnusedIncludes) {
                    recordMacroChange(Token, Definition.getMacroInfo() != nullptr);
                }
            }

            void MacroExpands(const Token& Token, const MacroDefinition& Definition, SourceRange,
                              const MacroArgs*) override {
                recordMacro(Token, Definition);
                const auto* Macro = Definition.getMacroInfo();
                // Pasted names have no unambiguous written token, even after
                // expansion. Do not infer unused dependencies from that gap.
                if (Macro && !llvm::any_of(Macro->tokens(), [](const auto& Token) {
                        return Token.is(tok::hashhash);
                    })) {
                    State.UnobservedMacros.erase(Macro);
                }
            }

            void Defined(const Token& Token, const MacroDefinition& Definition, SourceRange) override {
                recordMacro(Token, Definition);
            }

            void Ifdef(SourceLocation, const Token& Token, const MacroDefinition& Definition) override {
                recordMacro(Token, Definition);
            }

            void Ifndef(SourceLocation, const Token& Token, const MacroDefinition& Definition) override {
                recordMacro(Token, Definition);
            }

            void Elifdef(SourceLocation, const Token& Token, const MacroDefinition& Definition) override {
                recordMacro(Token, Definition);
            }

            void Elifndef(SourceLocation, const Token& Token, const MacroDefinition& Definition) override {
                recordMacro(Token, Definition);
            }
        };

        const llvm::SmallPtrSetImpl<File>& includedFiles(File Header) {
            auto Found = ReachableCache.find(Header);
            if (Found != ReachableCache.end()) {
                return Found->second;
            }
            auto Reachable = reachable(Header, DirectIncludes);
            auto& Result = ReachableCache[Header];
            Result.insert(Reachable.begin(), Reachable.end());
            return Result;
        }

        void markUsed(File Owner, File Provider) {
            // The include graph is complete before analysis starts. Each pair
            // needs one walk for the whole TU, including across separate owners,
            // repeated references and macros. This also terminates include cycles.
            if (Owner == Provider || !UsedIncludePaths.insert({Owner, Provider}).second) {
                return;
            }
            auto Found = DirectIncludes.find(Owner);
            if (Found == DirectIncludes.end()) {
                return;
            }
            for (File Header : Found->second) {
                if (includedFiles(Header).contains(Provider)) {
                    addUnique(UsedIncludes[Owner], Header);
                    markUsed(Header, Provider);
                }
            }
        }

        Files resolveConfigHeader(StringRef Name, StringRef Option = "Facades") {
            auto Normalized = normalizePath(Name);
            Files Result;
            for (const auto& Include : Includes) {
                const auto Physical = normalizePath(Include.Path);
                if (normalizePath(Include.Spelling) == Normalized || Physical == Normalized ||
                    StringRef(Physical).ends_with("/" + Normalized)) {
                    addUnique(Result, Include.Header);
                }
            }
            if (Result.size() > 1) {
                Check.diag("ambiguous header '%0' in %1 configuration; use a more specific path",
                           DiagnosticIDs::Error)
                    << Name << Option;
                InvalidConfiguration = true;
            }
            return Result;
        }

        template <typename ConfigType>
        bool readConfig(StringRef Text, StringRef Option, ConfigType& Config) {
            std::string Error;
            llvm::yaml::Input Input(
                Text, nullptr,
                [](const llvm::SMDiagnostic& Diag, void* Context) {
                    *static_cast<std::string*>(Context) = Diag.getMessage().str();
                },
                &Error);
            Input >> Config;
            if (Input.error()) {
                Check.diag("invalid %0 configuration: %1", DiagnosticIDs::Error) << Option << Error;
                InvalidConfiguration = true;
                return false;
            }
            return true;
        }

        void configureFacades() {
            FacadeMap Config;
            if (!readConfig(Facades, "Facades", Config)) {
                return;
            }
            for (const auto& Entry : Config) {
                auto FacadeFiles = resolveConfigHeader(Entry.first);
                if (FacadeFiles.size() != 1) {
                    continue; // Rules for facades outside this TU have no effect.
                }
                File Facade = FacadeFiles.front();
                auto Included = reachable(Facade, DirectIncludes);
                for (const auto& Target : Entry.second) {
                    auto TargetFiles = resolveConfigHeader(Target);
                    if (TargetFiles.size() != 1) {
                        continue;
                    }
                    if (!llvm::is_contained(Included, TargetFiles.front())) {
                        Check.diag("facade '%0' does not include exported header '%1' in this translation unit",
                                   DiagnosticIDs::Error)
                            << Entry.first << Target;
                        InvalidConfiguration = true;
                        continue;
                    }
                    addUnique(Exporters[TargetFiles.front()], Facade);
                    if (CheckUnusedIncludes) {
                        // The configured export is part of the facade's public API.
                        markUsed(Facade, TargetFiles.front());
                    }
                }
            }
        }

        std::string configHeaderSpelling(StringRef Name) {
            Name = Name.trim();
            bool Delimited = Name.starts_with("<") || Name.starts_with("\"");
            if (Name.empty() || Name.contains('\n') || Name.contains('\r') ||
                (Delimited && (Name.size() <= 2 ||
                               (Name.front() == '<' ? Name.back() != '>' : Name.back() != '"')))) {
                Check.diag("invalid header in SymbolProviders configuration: '%0'", DiagnosticIDs::Error) << Name;
                InvalidConfiguration = true;
                return {};
            }
            return Delimited ? Name.str() : "\"" + Name.str() + "\"";
        }

        void configureSymbolProviders() {
            std::map<std::string, SymbolProviderConfig> Config;
            if (!readConfig(SymbolProviders, "SymbolProviders", Config)) {
                return;
            }
            for (const auto& Entry : Config) {
                auto& Providers = ConfiguredProviders[Entry.first];
                Providers.Primary = configHeaderSpelling(Entry.second.Primary);
                auto AddHeader = [&](StringRef Spelling) {
                    if (Spelling.empty()) {
                        return;
                    }
                    for (File Header : resolveConfigHeader(Spelling, "SymbolProviders")) {
                        addUnique(Providers.Headers, Header);
                    }
                    if (Spelling.starts_with("<")) {
                        addPublicHeader(Providers.Headers, Spelling);
                    }
                };
                AddHeader(Providers.Primary);
                for (const auto& Secondary : Entry.second.Secondary) {
                    AddHeader(configHeaderSpelling(Secondary));
                }
            }
        }

        bool isMacroLocal(const Mention& Reference) const {
            if (!Reference.Location.isMacroID()) {
                return false;
            }
            for (const auto* Decl : Reference.Binding->redecls()) {
                if (sharesMacroExpansion(*SM, Reference.Location, Decl->getLocation())) {
                    return true;
                }
            }
            return false;
        }

        const Files& declarationFiles(const NamedDecl* Binding) {
            auto Found = DeclarationCache.find(Binding);
            if (Found != DeclarationCache.end()) {
                return Found->second;
            }
            Files Result;
            for (const auto* Decl : Binding->redecls()) {
                // Generated declarations belong where the macro expands, even when
                // the identifier itself was written in the replacement list.
                addUnique(Result, fileAt(SM->getExpansionLoc(Decl->getLocation())));
            }
            return DeclarationCache.try_emplace(Binding, std::move(Result)).first->second;
        }

        void addPublicHeader(Files& Origins, StringRef Name) const {
            llvm::SmallVector<StringRef, 4> Headers{Name};
            for (size_t Index = 0; Index < Headers.size(); ++Index) {
                StringRef Current = Headers[Index];
                for (const auto& Export : include_what_you_use::HeaderExports) {
                    if (Current == Export.Header && !llvm::is_contained(Headers, Export.Facade)) {
                        Headers.push_back(Export.Facade);
                    }
                }
                // The preprocessor indexes normalized angled spellings once.
                // Expand logical exports even if their source header is absent.
                auto Found = PublicHeaders.find(normalizePath(Current));
                if (Found != PublicHeaders.end()) {
                    for (File Header : Found->second) {
                        addUnique(Origins, Header);
                    }
                }
            }
        }

        void addDefaultProviders(const NamedDecl* Binding, ProviderSet& Result) {
            // Clang owns the C/C++ symbol table, including preferred headers,
            // alternatives, aliases, nested types and implementation namespaces.
            if (auto Symbol = StandardLibrary(Binding)) {
                if (auto Primary = Symbol->header()) {
                    Result.Primary = Primary->name().str();
                }
                for (auto Header : Symbol->headers()) {
                    addPublicHeader(Result.Headers, Header.name());
                }
                for (const auto& Provider : include_what_you_use::StandardSymbolProviders) {
                    if (Symbol->qualifiedName() == Provider.Name) {
                        addPublicHeader(Result.Headers, Provider.Header);
                    }
                }
            }
            if (!isa<TypeDecl>(Binding) || !Binding->getDeclContext()->getRedeclContext()->isTranslationUnit() ||
                !SM->isInSystemHeader(SM->getExpansionLoc(Binding->getLocation()))) {
                return;
            }
            // Public contracts remain valid when the active declaration is private
            // or an earlier include suppressed it through a shared typedef guard.
            if (const auto* Identifier = Binding->getIdentifier()) {
                for (const auto& Provider : include_what_you_use::SystemTypeProviders) {
                    if (Identifier->getName() == Provider.Name) {
                        addPublicHeader(Result.Headers, Provider.Header);
                        // The first supplemental entry for a symbol is its primary.
                        if (Result.Primary.empty()) {
                            Result.Primary = "<" + Provider.Header.str() + ">";
                        }
                    }
                }
            }
        }

        std::string symbolName(const NamedDecl* Binding) {
            if (auto Standard = StandardLibrary(Binding)) {
                // Do not replace a nested binding with its top-level container.
                if (!Binding->getDeclContext()->isRecord()) {
                    return Standard->qualifiedName().str();
                }
            }
            return Binding->getQualifiedNameAsString();
        }

        const ProviderSet& providers(const NamedDecl* Binding) {
            auto Cached = ProviderCache.find(Binding);
            if (Cached != ProviderCache.end()) {
                return Cached->second;
            }
            ProviderSet Result;
            Result.Headers = declarationFiles(Binding);
            addDefaultProviders(Binding, Result);
            auto Configured = ConfiguredProviders.find(symbolName(Binding));
            if (Configured != ConfiguredProviders.end()) {
                // A symbol contract adds providers and overrides the suggestion;
                // it does not revoke the declaration or standard providers.
                Result.Primary = Configured->second.Primary;
                for (File Header : Configured->second.Headers) {
                    if (CheckUnusedIncludes) {
                        for (File Origin : Result.Headers) {
                            markUsed(Header, Origin);
                        }
                    }
                }
                for (File Header : Configured->second.Headers) {
                    addUnique(Result.Headers, Header);
                }
            }
            Files Origins = std::move(Result.Headers);
            Result.Headers.clear();
            for (File Origin : Origins) {
                for (File Provider : reachable(Origin, Exporters)) {
                    addUnique(Result.Headers, Provider);
                }
            }
            return ProviderCache.try_emplace(Binding, std::move(Result)).first->second;
        }

        File requiredDefinition(const NamedDecl* Binding, bool RequiresDefinition) const {
            if (!RequiresDefinition) {
                return nullptr;
            }
            if (const auto* Template = dyn_cast<ClassTemplateDecl>(Binding)) {
                Binding = Template->getTemplatedDecl();
            }
            if (const auto* Tag = dyn_cast<TagDecl>(Binding)) {
                if (const auto* Definition = Tag->getDefinition()) {
                    return fileAt(SM->getExpansionLoc(Definition->getLocation()));
                }
            }
            return nullptr;
        }

        const Files& referenceProviders(const NamedDecl* Binding, File Definition) {
            const auto& Providers = providers(Binding).Headers;
            if (!Definition) {
                return Providers;
            }
            auto Cached = DefinitionProviderCache.find(Binding);
            if (Cached != DefinitionProviderCache.end()) {
                return Cached->second;
            }
            Files CompleteProviders;
            for (File Provider : Providers) {
                if (includedFiles(Provider).contains(Definition)) {
                    addUnique(CompleteProviders, Provider);
                }
            }
            // A direct forward declaration must not hide the transitive path
            // supplying a definition required by sizeof, a field or an object.
            return DefinitionProviderCache.try_emplace(Binding, std::move(CompleteProviders)).first->second;
        }

        Files referenceCarriers(File Owner, const Files& Providers) {
            Files Result;
            auto Direct = DirectIncludes.find(Owner);
            if (Direct == DirectIncludes.end()) {
                return Result;
            }
            for (File Header : Direct->second) {
                if (llvm::is_contained(Providers, Header)) {
                    addUnique(Result, Header);
                }
            }
            if (Result.empty()) {
                for (File Header : Direct->second) {
                    for (File Provider : Providers) {
                        if (Provider != Owner && includedFiles(Header).contains(Provider)) {
                            addUnique(Result, Header);
                            break;
                        }
                    }
                }
            }
            return Result;
        }

        void markReferencedProviders(File Owner, const Files& Providers) {
            for (File Header : referenceCarriers(Owner, Providers)) {
                addUnique(UsedIncludes[Owner], Header);
                if (!llvm::is_contained(Providers, Header)) {
                    // Retain the path inside an ordinary transitive include even
                    // if the written type also needs a direct-provider warning.
                    for (File Provider : Providers) {
                        markUsed(Header, Provider);
                    }
                }
            }
        }

        bool providesDeclarations(const Files& Carriers, const Files& Declarations) {
            if (Declarations.empty()) {
                return false;
            }
            for (File Declaration : Declarations) {
                bool Provided = false;
                for (File Header : Carriers) {
                    if (includedFiles(Header).contains(Declaration)) {
                        Provided = true;
                        break;
                    }
                }
                if (!Provided) {
                    return false;
                }
            }
            return true;
        }

        bool providesDeclarations(const Files& Carriers, const NamedDecl* Binding, bool RequiresDefinition = false) {
            if (File Definition = requiredDefinition(Binding, RequiresDefinition)) {
                return providesDeclarations(Carriers, {Definition});
            }
            return providesDeclarations(Carriers, declarationFiles(Binding));
        }

        void markIncluderProviders(SourceLocation Location, const Files& Providers, const Files& Declarations) {
            if (Declarations.empty()) {
                return;
            }
            Location = SM->getSpellingLoc(Location);
            while (Location.isValid()) {
                File Owner = fileAt(Location);
                if (!Owner || llvm::any_of(Declarations, [&](File Declaration) {
                        return includedFiles(Owner).contains(Declaration);
                    })) {
                    return;
                }
                // A non-self-contained header can depend on an earlier include
                // in its includer. Keep that dependency until the header is
                // repaired; its missing direct-provider warning still applies.
                Location = SM->getIncludeLoc(SM->getFileID(Location));
                if (File Includer = fileAt(Location)) {
                    markReferencedProviders(Includer, Providers);
                }
            }
        }

        void markReferenceProviders(const Mention& Reference) {
            if (isMacroLocal(Reference)) {
                return;
            }
            File Owner = fileAt(writtenLocation(*SM, Reference.Location));
            if (!Owner) {
                return;
            }
            File Definition = requiredDefinition(Reference.Binding, Reference.RequiresDefinition);
            const auto& Providers = referenceProviders(Reference.Binding, Definition);
            markReferencedProviders(Owner, Providers);
            auto Location = Reference.Location;
            while (Location.isMacroID()) {
                if (providesDeclarations({Owner}, Reference.Binding, Reference.RequiresDefinition)) {
                    return;
                }
                Location = SM->getImmediateMacroCallerLoc(Location);
                File Caller = fileAt(writtenLocation(*SM, Location));
                if (!Caller || Caller == Owner) {
                    continue;
                }
                // A wrapper may supply both the macro and its dependencies.
                // Credit those dependencies inside the wrapper, without keeping
                // an otherwise redundant include in the caller alive.
                auto Carriers = referenceCarriers(Caller, {Owner});
                if (providesDeclarations(Carriers, Reference.Binding, Reference.RequiresDefinition)) {
                    for (File Header : Carriers) {
                        markReferencedProviders(Header, Providers);
                    }
                    return;
                }
                // Non-self-contained macros rely on includes at their expansion
                // site. Retain those includes; missing-provider obligations still
                // belong to the spelling file and are diagnosed separately.
                markReferencedProviders(Caller, Providers);
                Owner = Caller;
            }
            if (Definition) {
                markIncluderProviders(Location, Providers, {Definition});
                return;
            }
            // Functions, namespaces and other names need an available
            // declaration, not every redeclaration encountered in this TU.
            // Logical providers can be valid even when a shared guard moved
            // the physical declaration to a different public header.
            markIncluderProviders(Location, Providers, Providers);
        }

        bool isAssociatedHeader(const Include& Include) const {
            StringRef Source = SM->getFilename(Include.Location);
            StringRef Extension = llvm::sys::path::extension(Source);
            return (Extension == ".cpp" || Extension == ".cc" || Extension == ".cxx" || Extension == ".C") &&
                   llvm::sys::path::stem(Source) == llvm::sys::path::stem(Include.Path);
        }

        void diagnoseUnused(const llvm::SmallVectorImpl<Mention>& References,
                            const InstantiationDependencies& Dependencies) {
            for (const auto& Reference : References) {
                markReferenceProviders(Reference);
            }
            llvm::SmallVector<Mention, 64> Pending(References.begin(), References.end());
            std::map<Files, llvm::SmallPtrSet<const NamedDecl*, 32>> Seen;
            while (!Pending.empty()) {
                auto Reference = Pending.pop_back_val();
                File Owner = fileAt(writtenLocation(*SM, Reference.Location));
                if (!Owner) {
                    continue;
                }
                // The same specialization can be expanded through different
                // macro wrappers and consuming files. Deduplicate by file chain,
                // not individual expansions, to retain each caller's dependencies.
                Files Owners{Owner};
                auto Location = Reference.Location;
                while (Location.isMacroID()) {
                    Location = SM->getImmediateMacroCallerLoc(Location);
                    addUnique(Owners, fileAt(writtenLocation(*SM, Location)));
                }
                if (!Seen[Owners].insert(Reference.Binding).second) {
                    continue;
                }
                auto Found = Dependencies.find(Reference.Binding);
                if (Found != Dependencies.end()) {
                    const auto Carriers = referenceCarriers(Owner, providers(Reference.Binding).Headers);
                    for (const auto& [Binding, RequiresDefinition] : Found->second) {
                        // Dependencies already supplied by the template's headers
                        // must not credit unrelated includes of the same internals.
                        // Still follow them: a nested template can call a user hook.
                        if (!providesDeclarations(Carriers, Binding, RequiresDefinition)) {
                            markReferenceProviders({Reference.Location, Binding, RequiresDefinition});
                        }
                        Pending.push_back({Reference.Location, Binding, RequiresDefinition});
                    }
                }
            }
            for (const auto& Reference : MacroReferences) {
                markUsed(fileAt(Reference.Location), Reference.Definition);
                markIncluderProviders(Reference.Location, {Reference.Definition}, {Reference.Definition});
            }
            llvm::SmallPtrSet<File, 32> UnobservedHeaders;
            for (const auto* Macro : UnobservedMacros) {
                if (!Macro->isUsedForHeaderGuard()) {
                    UnobservedHeaders.insert(fileAt(Macro->getDefinitionLoc()));
                }
            }
            for (const auto& Include : Includes) {
                if (Include.Keep || SM->isInSystemHeader(Include.Location) || isAssociatedHeader(Include) ||
                    UnobservedHeaders.contains(Include.Parent) || DiagnosticEffectHeaders.contains(Include.Header) ||
                    MacroCleanupHeaders.contains(Include.Header)) {
                    continue;
                }
                auto Used = UsedIncludes.find(Include.Parent);
                if (Used != UsedIncludes.end() && llvm::is_contained(Used->second, Include.Header)) {
                    continue;
                }
                Check.diag(Include.Location, "unused include '%0'") << Include.Spelling << Include.Range;
            }
        }

        // A preferred spelling identifies one edit, even for different types.
        // Without a preferred public header, keep each binding independently actionable.
        using GroupKey = std::pair<std::string, const NamedDecl*>;
        using Groups = std::map<GroupKey, llvm::SmallVector<Mention, 4>>;

        static tooling::IncludeStyle insertionStyle(const Groups& FileGroups) {
            // Give the proposed headers their own trailing category. HeaderIncludes
            // still handles comments, header guards and the limits of the leading block.
            std::string Proposed;
            for (const auto& [Key, Uses] : FileGroups) {
                if (!Key.first.empty()) {
                    if (!Proposed.empty()) {
                        Proposed += '|';
                    }
                    Proposed += llvm::Regex::escape(Key.first);
                }
            }
            auto Style = format::getLLVMStyle().IncludeStyle;
            // Existing includes also precede a proposed main header (priority 0).
            Style.IncludeCategories = {{"^(" + Proposed + ")$", 1, 1, true}, {".*", -1, -1, true}};
            return Style;
        }

        bool suppressedAt(SourceLocation Location) {
            // The primary warning moves to the include block. Query clang-tidy's
            // own NOLINT parser at the original use before aggregating it.
            DiagnosticStorage Storage;
            Diagnostic AtUse(&Context->getDiagnostics(), Location, 0, Storage, {});
            llvm::SmallVector<tooling::Diagnostic, 1> Errors;
            bool Suppressed =
                UseSuppressions.shouldSuppress(DiagnosticsEngine::Warning, AtUse, Check.getID(), Errors, true, true);
            for (const auto& Error : Errors) {
                TidyContext.diag(Error);
            }
            return Suppressed;
        }

        void collectMissing(const Mention& Use, llvm::DenseMap<File, Groups>& ByFile) {
            auto Location = writtenLocation(*SM, Use.Location);
            File Owner = fileAt(Location);
            if (!Owner || isMacroLocal(Use)) {
                return;
            }
            const auto& Declarations = declarationFiles(Use.Binding);
            if (Declarations.empty() || llvm::is_contained(Declarations, Owner)) {
                return;
            }
            const auto& Providers = providers(Use.Binding);
            auto Direct = DirectIncludes.find(Owner);
            if (Direct != DirectIncludes.end()) {
                for (File Header : Direct->second) {
                    if (llvm::is_contained(Providers.Headers, Header)) {
                        return;
                    }
                }
            }
            if (suppressedAt(Location)) {
                return;
            }
            auto& Group = ByFile[Owner][{Providers.Primary, Providers.Primary.empty() ? Use.Binding : nullptr}];
            Group.push_back({Location, Use.Binding});
        }

        std::optional<FixItHint> planInsertion(const tooling::HeaderIncludes& Inserter, FileID FID, StringRef Primary,
                                               ArrayRef<Mention> Uses) {
            bool Angled = Primary.starts_with("<");
            auto Name = Primary.drop_front().drop_back();
            if (Name.empty() || Name.find_first_of("\r\n<>\"") != StringRef::npos) {
                return std::nullopt;
            }
            auto Edit = Inserter.insert(Name, Angled, tooling::IncludeDirective::Include);
            if (!Edit) {
                return std::nullopt;
            }
            auto Site = SM->getLocForStartOfFile(FID).getLocWithOffset(Edit->getOffset());
            // Configuration supplied after the leading include block must stay
            // ahead of the provider, so keep the warning and omit that edit.
            auto Changes = PlacementBarriers.find(FID);
            unsigned NextChange = std::numeric_limits<unsigned>::max();
            if (Changes != PlacementBarriers.end()) {
                auto Next = llvm::lower_bound(Changes->second, Edit->getOffset());
                if (Next != Changes->second.end()) {
                    NextChange = *Next;
                }
            }
            if (llvm::any_of(Uses, [&](const Mention& Use) {
                    auto [UseFID, Offset] = SM->getDecomposedLoc(Use.Location);
                    return UseFID != FID || Offset < Edit->getOffset() || NextChange < Offset ||
                           Conditions->areInDifferentConditionalDirectiveRegion(Site, Use.Location);
                })) {
                return std::nullopt;
            }
            // A configured primary can be absent from the include search path,
            // or resolve relative to this header rather than to the main file.
            auto Owner = *SM->getFileEntryRefForID(FID);
            auto Target =
                PP->getHeaderSearchInfo().LookupFile(Name, Site, Angled, nullptr, nullptr, {{Owner, Owner.getDir()}},
                                                     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            if (!Target || includedFiles(&Target->getFileEntry()).contains(&Owner.getFileEntry())) {
                return std::nullopt;
            }
            auto Text = Edit->getReplacementText().str();
            if (SM->getBufferData(FID).contains("\r\n") && !StringRef(Text).contains("\r\n")) {
                llvm::SmallVector<StringRef, 3> Lines;
                StringRef(Text).split(Lines, '\n');
                Text = llvm::join(Lines, "\r\n");
            }
            return FixItHint::CreateInsertion(Site, Text);
        }

        void emitUseNotes(llvm::SmallVectorImpl<Mention>& Uses, StringRef Primary) {
            // RAV can reach the same spelling through several macro expansions.
            // Count unique written uses and select the first in physical source order.
            auto Position = [&](const Mention& Use) {
                return std::make_pair(SM->getFileOffset(Use.Location), Use.Binding);
            };
            llvm::stable_sort(Uses, [&](const Mention& A, const Mention& B) { return Position(A) < Position(B); });
            Uses.erase(std::unique(Uses.begin(), Uses.end(),
                                   [&](const Mention& A, const Mention& B) { return Position(A) == Position(B); }),
                       Uses.end());
            llvm::DenseMap<const NamedDecl*, unsigned> Counts;
            for (const auto& Use : Uses) {
                ++Counts[Use.Binding];
            }
            // With -line-filter even a later use can be the only changed line.
            // clang-tidy checks the note locations as well as the primary anchor.
            const bool LineFilter = !TidyContext.getGlobalOptions().LineFilter.empty();
            for (const auto& Use : Uses) {
                auto& Count = Counts[Use.Binding];
                if (!Count) {
                    if (LineFilter) {
                        Check.diag(Use.Location, "type '%0' is also used here", DiagnosticIDs::Note)
                            << symbolName(Use.Binding);
                    }
                    continue;
                }
                Check.diag(Use.Location, "type '%0' is used here (%1 %plural{1:use|:uses}1 in this file)",
                           DiagnosticIDs::Note)
                    << symbolName(Use.Binding) << Count;
                if (!Primary.empty()) {
                    Check.diag(Use.Location, "add #include %0 (primary provider)", DiagnosticIDs::Note) << Primary;
                }
                Check.diag(Use.Binding->getLocation(), "declaration is here", DiagnosticIDs::Note);
                Count = 0;
            }
        }

        void diagnoseMissing(ArrayRef<Mention> Mentions) {
            llvm::DenseMap<File, Groups> ByFile;
            for (const auto& Use : Mentions) {
                collectMissing(Use, ByFile);
            }
            for (auto& [Owner, FileGroups] : ByFile) {
                auto FID = SM->getFileID(FileGroups.begin()->second.front().Location);
                auto Start = SM->getLocForStartOfFile(FID);
                auto Anchor = FirstInclude.lookup(Owner);
                if (Anchor.isInvalid()) {
                    Anchor = Start;
                }
                std::optional<tooling::HeaderIncludes> Inserter;
                for (auto& [Key, Uses] : FileGroups) {
                    const auto& [Primary, Unresolved] = Key;
                    std::optional<FixItHint> Fix;
                    if (!Unresolved && !ManualIncludePlacement.contains(Owner) && !PP->isInNamedModule()) {
                        if (!Inserter) {
                            Inserter.emplace(SM->getFilename(Start), SM->getBufferData(FID), insertionStyle(FileGroups));
                        }
                        Fix = planInsertion(*Inserter, FID, Primary, Uses);
                    }
                    // A NOLINT on an unrelated existing include applies to that
                    // directive. Keep the actual edit at its own location.
                    auto Site = Fix ? Fix->RemoveRange.getBegin() : Anchor;
                    if (suppressedAt(Site)) {
                        Site = Uses.front().Location;
                    }
                    {
                        auto Warning = Check.diag(Site, Unresolved ? "missing direct declaration provider for type '%0'"
                                                                   : "missing direct include %0");
                        Warning << (Unresolved ? symbolName(Unresolved) : Primary);
                        if (Fix) {
                            Warning << *Fix;
                        }
                    }
                    emitUseNotes(Uses, Primary);
                }
            }
        }

        void finish() {
            if (!Context || Context->getDiagnostics().hasErrorOccurred()) {
                return;
            }
            configureFacades();
            configureSymbolProviders();
            if (InvalidConfiguration) {
                return;
            }
            llvm::SmallVector<Mention, 64> Mentions;
            llvm::SmallVector<const NamedDecl*, 64> Declarations;
            MentionVisitor(*SM, Mentions,
                           CheckUnusedIncludes && !ConfiguredProviders.empty() ? &Declarations : nullptr)
                .TraverseDecl(Context->getTranslationUnitDecl());
            // An exported symbol is part of a facade's API even when this client
            // does not reference it. Retain the includes that supply that symbol.
            for (const auto* Decl : Declarations) {
                if (ConfiguredProviders.count(symbolName(Decl))) {
                    providers(Decl);
                }
            }
            diagnoseMissing(Mentions);
            if (CheckUnusedIncludes) {
                // Written bindings retain aliases and using declarations. Add
                // semantic references without adding any missing-type obligations.
                llvm::SmallVector<Mention, 64> References(Mentions.begin(), Mentions.end());
                InstantiationDependencies Dependencies;
                ReferenceVisitor(*Context, References, Dependencies).TraverseDecl(Context->getTranslationUnitDecl());
                diagnoseUnused(References, Dependencies);
            }
        }
    };

    SdcIncludeWhatYouUseCheck::SdcIncludeWhatYouUseCheck(StringRef Name, ClangTidyContext* Context)
        : ClangTidyCheck(Name, Context), State(std::make_unique<Analysis>(*this, *Context, Options.get("Facades", "{}"),
                                                                          Options.get("SymbolProviders", "{}"),
                                                                          Options.get("CheckUnusedIncludes", true))) {}

    SdcIncludeWhatYouUseCheck::~SdcIncludeWhatYouUseCheck() = default;

    bool SdcIncludeWhatYouUseCheck::isLanguageVersionSupported(const LangOptions& LangOpts) const {
        return LangOpts.CPlusPlus;
    }

    void SdcIncludeWhatYouUseCheck::registerPPCallbacks(const SourceManager& SM, Preprocessor* PP,
                                                        Preprocessor*) {
        State->SM = &SM;
        State->PP = PP;
        auto Conditions = std::make_unique<PPConditionalDirectiveRecord>(PP->getSourceManager());
        State->Conditions = Conditions.get();
        PP->addPPCallbacks(std::move(Conditions));
        PP->addCommentHandler(State.get());
        PP->addPPCallbacks(std::make_unique<Analysis::IncludeRecorder>(*State));
    }

    void SdcIncludeWhatYouUseCheck::registerMatchers(ast_matchers::MatchFinder* Finder) {
        Finder->addMatcher(ast_matchers::translationUnitDecl().bind("translationUnit"), this);
    }

    void SdcIncludeWhatYouUseCheck::check(const ast_matchers::MatchFinder::MatchResult& Result) {
        State->Context = Result.Context;
    }

    void SdcIncludeWhatYouUseCheck::onEndOfTranslationUnit() {
        State->PP->removeCommentHandler(State.get());
        State->finish();
    }

    void SdcIncludeWhatYouUseCheck::storeOptions(ClangTidyOptions::OptionMap& Stored) {
        Options.store(Stored, "Facades", State->Facades);
        Options.store(Stored, "SymbolProviders", State->SymbolProviders);
        Options.store(Stored, "CheckUnusedIncludes", State->CheckUnusedIncludes);
    }

} // namespace clang::tidy::sdc
