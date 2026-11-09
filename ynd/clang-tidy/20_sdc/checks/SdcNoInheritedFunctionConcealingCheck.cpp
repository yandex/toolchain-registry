#include "SdcNoInheritedFunctionConcealingCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {
            namespace {

                bool isCopyOrMoveAssignment(const CXXMethodDecl* Method) {
                    return Method && Method->isOverloadedOperator() &&
                           Method->getOverloadedOperator() == OO_Equal && Method->getNumParams() == 1;
                }

                bool isRelevantBaseFunction(const CXXMethodDecl* Method) {
                    return Method && !isa<CXXConstructorDecl>(Method) && !isa<CXXDestructorDecl>(Method) &&
                           !isCopyOrMoveAssignment(Method) && !Method->getDeclName().isEmpty();
                }

                void collectBaseFunctions(const CXXRecordDecl* Record,
                                          SmallVectorImpl<const CXXMethodDecl*>& Functions) {
                    if (!Record || !Record->hasDefinition()) {
                        return;
                    }

                    for (const CXXBaseSpecifier& Base : Record->bases()) {
                        if (Base.getAccessSpecifier() == AS_private) {
                            continue;
                        }

                        const Type* BaseType = Base.getType().getTypePtrOrNull();
                        const CXXRecordDecl* BaseRecord = BaseType ? BaseType->getAsCXXRecordDecl() : nullptr;
                        if (!BaseRecord || !BaseRecord->hasDefinition()) {
                            continue;
                        }

                        BaseRecord = BaseRecord->getDefinition();
                        collectBaseFunctions(BaseRecord, Functions);
                        for (const Decl* Declaration : BaseRecord->decls()) {
                            if (const auto* Method = dyn_cast<CXXMethodDecl>(Declaration)) {
                                if (isRelevantBaseFunction(Method)) {
                                    Functions.push_back(Method->getCanonicalDecl());
                                }
                            } else if (const auto* FunctionTemplate = dyn_cast<FunctionTemplateDecl>(Declaration)) {
                                const auto* TemplateMethod = dyn_cast<CXXMethodDecl>(FunctionTemplate->getTemplatedDecl());
                                if (isRelevantBaseFunction(TemplateMethod)) {
                                    Functions.push_back(TemplateMethod->getCanonicalDecl());
                                }
                            }
                        }
                    }
                }

                bool isDerivedFunction(const NamedDecl* Declaration) {
                    if (const auto* Method = dyn_cast<CXXMethodDecl>(Declaration)) {
                        return !Method->isImplicit() && !isa<CXXConstructorDecl>(Method) && !isa<CXXDestructorDecl>(Method);
                    }
                    if (const auto* FunctionTemplate = dyn_cast<FunctionTemplateDecl>(Declaration)) {
                        const auto* Method = dyn_cast<CXXMethodDecl>(FunctionTemplate->getTemplatedDecl());
                        return Method && !Method->isImplicit();
                    }
                    return false;
                }

                bool isDerivedVariable(const NamedDecl* Declaration) {
                    return isa<FieldDecl>(Declaration) || isa<VarDecl>(Declaration);
                }

                const FunctionDecl* getAsFunctionDecl(const NamedDecl* Declaration) {
                    if (const auto* Function = dyn_cast<FunctionDecl>(Declaration)) {
                        return Function;
                    }
                    if (const auto* FunctionTemplate = dyn_cast<FunctionTemplateDecl>(Declaration)) {
                        return FunctionTemplate->getTemplatedDecl();
                    }
                    return nullptr;
                }

                bool overridesBaseFunction(const NamedDecl* Declaration, const CXXMethodDecl* BaseFunction) {
                    const auto* Method = dyn_cast_or_null<CXXMethodDecl>(getAsFunctionDecl(Declaration));
                    if (!Method || !BaseFunction->isVirtual()) {
                        return false;
                    }

                    // Clang records direct overrides only, while BaseFunction may
                    // belong to any ancestor collected by collectBaseFunctions.
                    SmallVector<const CXXMethodDecl*, 8> Pending(Method->overridden_methods());
                    llvm::DenseSet<const CXXMethodDecl*> Visited;
                    while (!Pending.empty()) {
                        const CXXMethodDecl* Overridden = Pending.pop_back_val()->getCanonicalDecl();
                        if (!Visited.insert(Overridden).second) {
                            continue;
                        }
                        if (Overridden == BaseFunction->getCanonicalDecl()) {
                            return true;
                        }
                        for (const CXXMethodDecl* Ancestor : Overridden->overridden_methods()) {
                            Pending.push_back(Ancestor);
                        }
                    }
                    return false;
                }

                llvm::DenseSet<const CXXMethodDecl*> collectUsingIntroducedFunctions(const CXXRecordDecl* Record) {
                    llvm::DenseSet<const CXXMethodDecl*> Functions;
                    for (const Decl* Declaration : Record->decls()) {
                        const auto* Using = dyn_cast<UsingDecl>(Declaration);
                        if (!Using) {
                            continue;
                        }

                        for (const UsingShadowDecl* Shadow : Using->shadows()) {
                            const NamedDecl* Target = Shadow->getTargetDecl();
                            if (const auto* Method = dyn_cast<CXXMethodDecl>(Target)) {
                                Functions.insert(Method->getCanonicalDecl());
                            } else if (const auto* FunctionTemplate = dyn_cast<FunctionTemplateDecl>(Target)) {
                                if (const auto* Method = dyn_cast<CXXMethodDecl>(FunctionTemplate->getTemplatedDecl())) {
                                    Functions.insert(Method->getCanonicalDecl());
                                }
                            }
                        }
                    }
                    return Functions;
                }

                llvm::DenseMap<DeclarationName, SmallVector<const NamedDecl*, 2>> collectDerivedConcealers(const CXXRecordDecl* Record) {
                    llvm::DenseMap<DeclarationName, SmallVector<const NamedDecl*, 2>> Concealers;
                    for (const Decl* Declaration : Record->decls()) {
                        const auto* Named = dyn_cast<NamedDecl>(Declaration);
                        if (!Named || Named->getDeclName().isEmpty()) {
                            continue;
                        }

                        if (isDerivedFunction(Named) || isDerivedVariable(Named)) {
                            Concealers[Named->getDeclName()].push_back(Named);
                        }
                    }
                    return Concealers;
                }

            } // namespace

            SdcNoInheritedFunctionConcealingCheck::SdcNoInheritedFunctionConcealingCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoInheritedFunctionConcealingCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(
                    cxxRecordDecl(
                        isDefinition(),
                        unless(isExpansionInSystemHeader()),
                        unless(isImplicit()))
                        .bind("record"),
                    this);
            }

            void SdcNoInheritedFunctionConcealingCheck::check(const MatchFinder::MatchResult& Result) {
                const auto* Record = Result.Nodes.getNodeAs<CXXRecordDecl>("record");
                if (!Record || !Record->hasDefinition() || Record->bases().empty()) {
                    return;
                }

                SmallVector<const CXXMethodDecl*, 16> BaseFunctions;
                collectBaseFunctions(Record, BaseFunctions);
                if (BaseFunctions.empty()) {
                    return;
                }

                llvm::DenseSet<const CXXMethodDecl*> IntroducedByUsing = collectUsingIntroducedFunctions(Record);
                llvm::DenseMap<DeclarationName, SmallVector<const NamedDecl*, 2>> Concealers = collectDerivedConcealers(Record);

                llvm::DenseSet<const CXXMethodDecl*> Reported;
                llvm::DenseMap<const NamedDecl*, SmallVector<const CXXMethodDecl*, 4>> Hidden;
                SmallVector<const NamedDecl*, 8> Order;
                for (const CXXMethodDecl* BaseFunction : BaseFunctions) {
                    if (!BaseFunction || IntroducedByUsing.contains(BaseFunction->getCanonicalDecl()) ||
                        Reported.contains(BaseFunction->getCanonicalDecl())) {
                        continue;
                    }

                    auto Found = Concealers.find(BaseFunction->getDeclName());
                    if (Found == Concealers.end()) {
                        continue;
                    }

                    const NamedDecl* Concealer = nullptr;
                    bool HasOverride = false;
                    for (const NamedDecl* Candidate : Found->second) {
                        if (overridesBaseFunction(Candidate, BaseFunction)) {
                            HasOverride = true;
                            break;
                        }
                        if (!Concealer) {
                            Concealer = Candidate;
                        }
                    }

                    if (HasOverride || !Concealer) {
                        continue;
                    }

                    if (!Hidden.count(Concealer)) Order.push_back(Concealer);
                    Hidden[Concealer].push_back(BaseFunction);
                    Reported.insert(BaseFunction->getCanonicalDecl());
                }
                for (const NamedDecl* Concealer : Order) {
                    diag(Concealer->getLocation(), "'%0' conceals inherited function overloads named '%1'")
                        << Concealer->getQualifiedNameAsString() << Concealer->getNameAsString();
                    for (const CXXMethodDecl* Base : Hidden[Concealer])
                        diag(Base->getLocation(), "concealed base function '%0' has type %1", DiagnosticIDs::Note)
                            << Base->getQualifiedNameAsString() << Base->getType();
                }
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
