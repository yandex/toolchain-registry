#include "SdcNoDependentBaseUnqualifiedLookupCheck.h"
#include "SdcCodeSelection.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Expr.h"
#include "clang/AST/TypeLoc.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSet.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {
            namespace {

                bool isNamedClassMember(const NamedDecl* Declaration) {
                    return Declaration && Declaration->getIdentifier() &&
                           (isa<FieldDecl>(Declaration) || isa<VarDecl>(Declaration) || isa<CXXMethodDecl>(Declaration) ||
                            isa<FunctionTemplateDecl>(Declaration) || isa<TypeDecl>(Declaration) || isa<EnumConstantDecl>(Declaration));
                }

                const CXXRecordDecl* instantiatedPattern(
                    const ClassTemplateSpecializationDecl& Specialization) {
                    auto From = Specialization.getInstantiatedFrom();
                    if (auto* Primary = From.dyn_cast<ClassTemplateDecl*>()) {
                        return Primary->getTemplatedDecl();
                    }
                    return From.dyn_cast<ClassTemplatePartialSpecializationDecl*>();
                }

                void collectClassAndBaseNames(
                    const CXXRecordDecl* Record, llvm::StringSet<>& Names,
                    llvm::SmallPtrSet<const CXXRecordDecl*, 32>& Visited) {
                    if (!Record || !Record->hasDefinition()) {
                        return;
                    }
                    Record = Record->getDefinition();
                    if (!Visited.insert(Record).second) {
                        return;
                    }

                    for (const Decl* Declaration : Record->decls()) {
                        if (const auto* Named = dyn_cast<NamedDecl>(Declaration)) {
                            if (isNamedClassMember(Named)) {
                                Names.insert(Named->getName());
                            }
                        }
                    }
                    for (const CXXBaseSpecifier& Base : Record->bases()) {
                        collectClassAndBaseNames(
                            Base.getType()->getAsCXXRecordDecl(), Names,
                            Visited);
                    }
                }

                llvm::StringSet<> collectConcreteDependentBaseNames(
                    const ClassTemplateSpecializationDecl& Specialization,
                    const CXXRecordDecl& Pattern) {
                    llvm::StringSet<> Names;
                    llvm::SmallPtrSet<const CXXRecordDecl*, 32> Visited;
                    auto PatternBase = Pattern.bases_begin();
                    auto ConcreteBase = Specialization.bases_begin();
                    while (PatternBase != Pattern.bases_end() &&
                           ConcreteBase != Specialization.bases_end()) {
                        if (PatternBase->getType()->isDependentType()) {
                            collectClassAndBaseNames(
                                ConcreteBase->getType()->getAsCXXRecordDecl(),
                                Names, Visited);
                        }
                        ++PatternBase;
                        ++ConcreteBase;
                    }
                    return Names;
                }

                llvm::StringSet<> collectUsingIntroducedNames(const CXXRecordDecl* Record) {
                    llvm::StringSet<> Names;
                    for (const Decl* Declaration : Record->decls()) {
                        const auto* Using = dyn_cast<UsingDecl>(Declaration);
                        if (!Using) {
                            continue;
                        }

                        for (const UsingShadowDecl* Shadow : Using->shadows()) {
                            if (const NamedDecl* Target = Shadow->getTargetDecl()) {
                                if (Target->getIdentifier()) {
                                    Names.insert(Target->getName());
                                }
                            }
                        }
                    }
                    return Names;
                }

                class UnqualifiedLookupVisitor: public RecursiveASTVisitor<UnqualifiedLookupVisitor> {
                public:
                    UnqualifiedLookupVisitor(SdcNoDependentBaseUnqualifiedLookupCheck& Check,
                                             llvm::StringSet<> DependentBaseNames,
                                             llvm::StringSet<> UsingIntroducedNames,
                                             const CXXRecordDecl& Record,
                                             ASTContext& Context)
                        : Check_(Check), DependentBaseNames_(std::move(DependentBaseNames)),
                          UsingIntroducedNames_(std::move(UsingIntroducedNames)),
                          Record_(Record), Context_(Context)
                    {
                    }

                    bool VisitDeclRefExpr(DeclRefExpr* Expr) {
                        if (!Expr || Expr->getQualifier() || !Expr->getFoundDecl() || !Expr->getFoundDecl()->getIdentifier()) {
                            return true;
                        }

                        reportIfDependentBaseName(Expr->getFoundDecl(),
                                                  Expr->getLocation());
                        return true;
                    }

                    bool VisitMemberExpr(MemberExpr* Expr) {
                        if (!Expr || Expr->getQualifier() || Expr->isArrow() || !Expr->isImplicitAccess() ||
                            !Expr->getMemberDecl() || !Expr->getMemberDecl()->getIdentifier()) {
                            return true;
                        }

                        reportIfDependentBaseName(Expr->getMemberDecl(),
                                                  Expr->getMemberLoc());
                        return true;
                    }

                    bool VisitTypedefTypeLoc(TypedefTypeLoc TypeLocation) {
                        if (TypeLocation.getBeginLoc() !=
                            TypeLocation.getNameLoc()) {
                            return true;
                        }
                        reportIfDependentBaseName(
                            TypeLocation.getTypedefNameDecl(),
                            TypeLocation.getNameLoc());
                        return true;
                    }

                    bool VisitTagTypeLoc(TagTypeLoc TypeLocation) {
                        if (TypeLocation.getBeginLoc() !=
                            TypeLocation.getNameLoc()) {
                            return true;
                        }
                        reportIfDependentBaseName(TypeLocation.getDecl(),
                                                  TypeLocation.getNameLoc());
                        return true;
                    }

                private:
                    bool isLocalOrCurrentClassEntity(
                        const NamedDecl& Declaration) const {
                        const DeclContext* Context = Declaration.getDeclContext();
                        if (!Context)
                            return false;
                        if (Context->isFunctionOrMethod())
                            return true;
                        if (Context == &Record_)
                            return true;
                        if (const auto* RecordContext =
                                dyn_cast<CXXRecordDecl>(Context)) {
                            if (RecordContext->getCanonicalDecl() ==
                                Record_.getCanonicalDecl()) {
                                return true;
                            }
                        }
                        return false;
                    }

                    void reportIfDependentBaseName(
                        const NamedDecl* Resolved, SourceLocation Location) {
                        if (!Resolved || !Resolved->getIdentifier())
                            return;
                        const StringRef Name = Resolved->getName();
                        if (UsingIntroducedNames_.contains(Name) || !DependentBaseNames_.contains(Name)) {
                            return;
                        }
                        if (isLocalOrCurrentClassEntity(*Resolved) ||
                            !isWrittenInAnalyzedSource(
                                Location, Context_.getSourceManager())) {
                            return;
                        }

                        Check_.diag(Location,
                                    "name present in a dependent base shall not be resolved by unqualified lookup");
                    }

                    SdcNoDependentBaseUnqualifiedLookupCheck& Check_;
                    llvm::StringSet<> DependentBaseNames_;
                    llvm::StringSet<> UsingIntroducedNames_;
                    const CXXRecordDecl& Record_;
                    ASTContext& Context_;
                };

            } // namespace

            SdcNoDependentBaseUnqualifiedLookupCheck::SdcNoDependentBaseUnqualifiedLookupCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoDependentBaseUnqualifiedLookupCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    cxxMethodDecl(
                        isDefinition(),
                        unless(isExpansionInSystemHeader()))
                        .bind("method"),
                    this);
            }

            void SdcNoDependentBaseUnqualifiedLookupCheck::check(const MatchFinder::MatchResult& Result) {
                const auto* Method =
                    Result.Nodes.getNodeAs<CXXMethodDecl>("method");
                if (!Method || !Method->doesThisDeclarationHaveABody() ||
                    !::clang::isTemplateInstantiation(
                        Method->getTemplateSpecializationKind())) {
                    return;
                }

                const auto* Record = dyn_cast<ClassTemplateSpecializationDecl>(
                    Method->getParent());
                if (!Record || !Record->hasDefinition())
                    return;
                const CXXRecordDecl* Pattern = instantiatedPattern(*Record);
                if (!Pattern || !Pattern->hasDefinition())
                    return;
                Pattern = Pattern->getDefinition();

                llvm::StringSet<> DependentBaseNames =
                    collectConcreteDependentBaseNames(*Record, *Pattern);
                if (DependentBaseNames.empty()) {
                    return;
                }

                llvm::StringSet<> UsingNames =
                    collectUsingIntroducedNames(Record);
                llvm::StringSet<> PatternUsingNames =
                    collectUsingIntroducedNames(Pattern);
                for (const auto& Name : PatternUsingNames)
                    UsingNames.insert(Name.getKey());

                UnqualifiedLookupVisitor Visitor(
                    *this, std::move(DependentBaseNames),
                    std::move(UsingNames), *Record, *Result.Context);
                Visitor.TraverseStmt(Method->getBody());
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
