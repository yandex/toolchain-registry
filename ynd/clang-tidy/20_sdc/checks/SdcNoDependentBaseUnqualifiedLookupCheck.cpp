#include "SdcNoDependentBaseUnqualifiedLookupCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
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

                const CXXRecordDecl* dependentBasePattern(const CXXBaseSpecifier& Base) {
                    const Type* BaseType = Base.getType().getTypePtrOrNull();
                    if (!BaseType || !BaseType->isDependentType()) {
                        return nullptr;
                    }

                    if (const auto* TemplateSpecialization = BaseType->getAs<TemplateSpecializationType>()) {
                        TemplateName Name = TemplateSpecialization->getTemplateName();
                        if (const TemplateDecl* Template = Name.getAsTemplateDecl()) {
                            return dyn_cast<CXXRecordDecl>(Template->getTemplatedDecl());
                        }
                    }

                    if (const auto* Injected = BaseType->getAs<InjectedClassNameType>()) {
                        return Injected->getDecl();
                    }

                    return nullptr;
                }

                void collectDependentBaseNames(const CXXRecordDecl* Record, llvm::StringSet<>& Names) {
                    if (!Record || !Record->hasDefinition()) {
                        return;
                    }

                    for (const CXXBaseSpecifier& Base : Record->bases()) {
                        const CXXRecordDecl* Pattern = dependentBasePattern(Base);
                        if (!Pattern || !Pattern->hasDefinition()) {
                            continue;
                        }

                        Pattern = Pattern->getDefinition();
                        collectDependentBaseNames(Pattern, Names);
                        for (const Decl* Declaration : Pattern->decls()) {
                            if (const auto* Named = dyn_cast<NamedDecl>(Declaration)) {
                                if (isNamedClassMember(Named)) {
                                    Names.insert(Named->getName());
                                }
                            }
                        }
                    }
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
                                             llvm::StringSet<> UsingIntroducedNames)
                        : Check_(Check), DependentBaseNames_(std::move(DependentBaseNames)),
                          UsingIntroducedNames_(std::move(UsingIntroducedNames))
                    {
                    }

                    bool VisitDeclRefExpr(DeclRefExpr* Expr) {
                        if (!Expr || Expr->getQualifier() || !Expr->getFoundDecl() || !Expr->getFoundDecl()->getIdentifier()) {
                            return true;
                        }

                        reportIfDependentBaseName(Expr->getFoundDecl()->getName(), Expr->getLocation());
                        return true;
                    }

                    bool VisitMemberExpr(MemberExpr* Expr) {
                        if (!Expr || Expr->getQualifier() || Expr->isArrow() || !Expr->isImplicitAccess() ||
                            !Expr->getMemberDecl() || !Expr->getMemberDecl()->getIdentifier()) {
                            return true;
                        }

                        reportIfDependentBaseName(Expr->getMemberDecl()->getName(), Expr->getMemberLoc());
                        return true;
                    }

                private:
                    void reportIfDependentBaseName(StringRef Name, SourceLocation Location) {
                        if (UsingIntroducedNames_.contains(Name) || !DependentBaseNames_.contains(Name)) {
                            return;
                        }

                        Check_.diag(Location,
                                    "name present in a dependent base shall not be resolved by unqualified lookup");
                    }

                    SdcNoDependentBaseUnqualifiedLookupCheck& Check_;
                    llvm::StringSet<> DependentBaseNames_;
                    llvm::StringSet<> UsingIntroducedNames_;
                };

            } // namespace

            SdcNoDependentBaseUnqualifiedLookupCheck::SdcNoDependentBaseUnqualifiedLookupCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoDependentBaseUnqualifiedLookupCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    cxxRecordDecl(
                        isDefinition(),
                        unless(isExpansionInSystemHeader()))
                        .bind("record"),
                    this);
            }

            void SdcNoDependentBaseUnqualifiedLookupCheck::check(const MatchFinder::MatchResult& Result) {
                const auto* Record = Result.Nodes.getNodeAs<CXXRecordDecl>("record");
                if (!Record || !Record->hasDefinition() || !Record->isDependentContext()) {
                    return;
                }

                llvm::StringSet<> DependentBaseNames;
                collectDependentBaseNames(Record, DependentBaseNames);
                if (DependentBaseNames.empty()) {
                    return;
                }

                UnqualifiedLookupVisitor Visitor(*this, std::move(DependentBaseNames), collectUsingIntroducedNames(Record));
                for (Decl* Declaration : Record->decls()) {
                    if (auto* Method = dyn_cast<CXXMethodDecl>(Declaration)) {
                        if (Method->doesThisDeclarationHaveABody()) {
                            Visitor.TraverseStmt(Method->getBody());
                        }
                    } else if (auto* FunctionTemplate = dyn_cast<FunctionTemplateDecl>(Declaration)) {
                        if (auto* Method = dyn_cast<CXXMethodDecl>(FunctionTemplate->getTemplatedDecl())) {
                            if (Method->doesThisDeclarationHaveABody()) {
                                Visitor.TraverseStmt(Method->getBody());
                            }
                        }
                    }
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
