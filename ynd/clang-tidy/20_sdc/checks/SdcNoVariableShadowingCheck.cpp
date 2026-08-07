#include "SdcNoVariableShadowingCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {
            namespace {

                bool isVariableLikeDecl(const NamedDecl* Declaration) {
                    return isa<VarDecl>(Declaration) || isa<FieldDecl>(Declaration) ||
                           isa<ParmVarDecl>(Declaration) || isa<EnumConstantDecl>(Declaration);
                }

                class ShadowingVisitor: public RecursiveASTVisitor<ShadowingVisitor> {
                public:
                    ShadowingVisitor(SdcNoVariableShadowingCheck& Check, ASTContext& Context)
                        : Check_(Check), Context_(Context), SourceMgr_(Context.getSourceManager())
                    {
                        pushScope();
                    }

                    bool TraverseNamespaceDecl(NamespaceDecl* Declaration) {
                        if (!Declaration) {
                            return true;
                        }

                        pushScope();
                        bool Result = RecursiveASTVisitor<ShadowingVisitor>::TraverseNamespaceDecl(Declaration);
                        popScope();
                        return Result;
                    }

                    bool TraverseCXXRecordDecl(CXXRecordDecl* Declaration) {
                        if (!Declaration || !Declaration->isThisDeclarationADefinition() || Declaration->isImplicit()) {
                            return true;
                        }

                        pushScope(); ClassScopeDepth_++;
                        addBaseClassMembers(Declaration);
                        pushScope(); ClassScopeDepth_++;
                        bool Result = RecursiveASTVisitor<ShadowingVisitor>::TraverseCXXRecordDecl(Declaration);
                        popScope(); ClassScopeDepth_--;
                        popScope(); ClassScopeDepth_--;
                        return Result;
                    }

                    bool TraverseFunctionDecl(FunctionDecl* Declaration) {
                        return traverseFunctionDecl(Declaration);
                    }

                    bool TraverseCXXMethodDecl(CXXMethodDecl* Declaration) {
                        return traverseFunctionDecl(Declaration);
                    }

                    bool TraverseCompoundStmt(CompoundStmt* Statement) {
                        if (!Statement) {
                            return true;
                        }

                        if (Statement == FunctionBody_) {
                            return RecursiveASTVisitor<ShadowingVisitor>::TraverseCompoundStmt(Statement);
                        }

                        pushScope();
                        bool Result = RecursiveASTVisitor<ShadowingVisitor>::TraverseCompoundStmt(Statement);
                        popScope();
                        return Result;
                    }

                    bool TraverseIfStmt(IfStmt* Statement) {
                        pushScope();
                        bool Result = RecursiveASTVisitor<ShadowingVisitor>::TraverseIfStmt(Statement);
                        popScope();
                        return Result;
                    }

                    bool TraverseSwitchStmt(SwitchStmt* Statement) {
                        pushScope();
                        bool Result = RecursiveASTVisitor<ShadowingVisitor>::TraverseSwitchStmt(Statement);
                        popScope();
                        return Result;
                    }

                    bool TraverseForStmt(ForStmt* Statement) {
                        pushScope();
                        bool Result = RecursiveASTVisitor<ShadowingVisitor>::TraverseForStmt(Statement);
                        popScope();
                        return Result;
                    }

                    bool TraverseCXXForRangeStmt(CXXForRangeStmt* Statement) {
                        pushScope();
                        bool Result = RecursiveASTVisitor<ShadowingVisitor>::TraverseCXXForRangeStmt(Statement);
                        popScope();
                        return Result;
                    }

                    bool TraverseWhileStmt(WhileStmt* Statement) {
                        pushScope();
                        bool Result = RecursiveASTVisitor<ShadowingVisitor>::TraverseWhileStmt(Statement);
                        popScope();
                        return Result;
                    }

                    bool TraverseDoStmt(DoStmt* Statement) {
                        pushScope();
                        bool Result = RecursiveASTVisitor<ShadowingVisitor>::TraverseDoStmt(Statement);
                        popScope();
                        return Result;
                    }

                    bool TraverseCXXCatchStmt(CXXCatchStmt* Statement) {
                        if (!Statement) {
                            return true;
                        }
                        pushScope();
                        if (VarDecl* Exception = Statement->getExceptionDecl()) {
                            handleDeclaration(Exception);
                        }
                        bool Result = TraverseStmt(Statement->getHandlerBlock());
                        popScope();
                        return Result;
                    }

                    bool TraverseLambdaExpr(LambdaExpr* Expression) {
                        if (!Expression) {
                            return true;
                        }

                        llvm::SmallPtrSet<const ValueDecl*, 8> CapturedVariables;
                        bool CapturesThis = false;
                        for (const LambdaCapture& Capture : Expression->captures()) {
                            if (Capture.capturesVariable()) {
                                CapturedVariables.insert(Capture.getCapturedVar());
                            } else if (Capture.capturesThis()) {
                                CapturesThis = true;
                            }
                        }

                        // A lambda does not inherit ordinary automatic locals
                        // unless they are captured. It can still name globals,
                        // static/thread locals, usable constant-expression
                        // variables, enumerators, and fields through captured
                        // this. Build exactly that visible outer scope.
                        llvm::StringMap<const NamedDecl*> VisibleOuter;
                        for (const auto& Scope : Scopes_) {
                            for (const auto& Entry : Scope) {
                                const NamedDecl* Declaration = Entry.second;
                                bool Visible = isa<EnumConstantDecl>(Declaration);
                                if (const auto* Variable =
                                        dyn_cast<VarDecl>(Declaration)) {
                                    Visible = Variable->hasGlobalStorage() ||
                                              Variable->isUsableInConstantExpressions(Context_) ||
                                              CapturedVariables.contains(Variable);
                                } else if (isa<FieldDecl>(Declaration)) {
                                    Visible = CapturesThis;
                                }
                                if (Visible) {
                                    VisibleOuter[Entry.getKey()] = Declaration;
                                }
                            }
                        }

                        auto SavedScopes = std::move(Scopes_);
                        Scopes_.clear();
                        Scopes_.push_back(std::move(VisibleOuter));
                        pushScope();
                        if (const CXXMethodDecl* CallOperator =
                                Expression->getCallOperator()) {
                            for (const ParmVarDecl* Param :
                                 CallOperator->parameters()) {
                                handleDeclaration(Param);
                            }
                        }
                        bool Result = TraverseStmt(Expression->getBody());
                        popScope();
                        Scopes_ = std::move(SavedScopes);
                        return Result;
                    }

                    bool TraverseVarDecl(VarDecl* Declaration) {
                        handleDeclaration(Declaration);
                        // Initializers can introduce nested scopes (notably a
                        // lambda body) that must be visited after the variable
                        // itself has entered the current scope.
                        return !Declaration || !Declaration->hasInit() ||
                               TraverseStmt(Declaration->getInit());
                    }

                    bool TraverseFieldDecl(FieldDecl* Declaration) {
                        handleDeclaration(Declaration);
                        return true;
                    }

                    bool TraverseParmVarDecl(ParmVarDecl* Declaration) {
                        // Parameters of function *definitions* are added to the
                        // scope manually inside traverseFunctionDecl(), which also
                        // controls scope creation.  TraverseParmVarDecl reaches
                        // only parameters that fall outside that path — chiefly
                        // function-pointer typedef parameters such as:
                        //   typedef void(*F)(int options);
                        // Those parameter names are purely documentary; they occupy
                        // no real scope and must not be registered here, or they
                        // would cause false "shadows" for any local variable with
                        // the same name.
                        return true;
                    }

                    bool TraverseEnumDecl(EnumDecl* Declaration) {
                        if (!Declaration || Declaration->isScoped()) {
                            return true;
                        }

                        for (EnumConstantDecl* Enumerator : Declaration->enumerators()) {
                            handleDeclaration(Enumerator);
                        }
                        return true;
                    }

                    bool TraverseUsingDecl(UsingDecl* Declaration) {
                        if (!Declaration) {
                            return true;
                        }

                        for (const UsingShadowDecl* Shadow : Declaration->shadows()) {
                            const NamedDecl* Target = Shadow->getTargetDecl();
                            if (isVariableLikeDecl(Target) && Target->getIdentifier()) {
                                Scopes_.back()[Target->getName()] = Target;
                            }
                        }
                        return true;
                    }

                private:
                    void pushScope() {
                        Scopes_.emplace_back();
                    }

                    void popScope() {
                        Scopes_.pop_back();
                    }

                    bool traverseFunctionDecl(FunctionDecl* Declaration) {
                        if (!Declaration || !Declaration->doesThisDeclarationHaveABody()) {
                            return true;
                        }

                        // A non-member function (e.g. an inline friend) defined
                        // inside a class body does NOT have class members in
                        // scope — unqualified lookup never searches the enclosing
                        // class for non-members.  Temporarily hide the
                        // class-context scopes so that parameters/locals of this
                        // function are not incorrectly flagged as shadowing them.
                        SmallVector<llvm::StringMap<const NamedDecl*>, 4> HiddenClassScopes;
                        if (!isa<CXXMethodDecl>(Declaration) && ClassScopeDepth_ > 0) {
                            for (size_t i = 0; i < ClassScopeDepth_; ++i) {
                                HiddenClassScopes.push_back(std::move(Scopes_.back()));
                                Scopes_.pop_back();
                            }
                        }

                        bool AddedClassScope = false;
                        if (const auto* Method = dyn_cast<CXXMethodDecl>(Declaration)) {
                            if (const CXXRecordDecl* Parent = Method->getParent()) {
                                pushScope();
                                addClassMembers(Parent);
                                AddedClassScope = true;
                            }
                        }

                        pushScope();
                        for (const ParmVarDecl* Param : Declaration->parameters()) {
                            handleDeclaration(Param);
                        }

                        const Stmt* PreviousFunctionBody = FunctionBody_;
                        FunctionBody_ = Declaration->getBody();
                        TraverseStmt(const_cast<Stmt*>(Declaration->getBody()));
                        FunctionBody_ = PreviousFunctionBody;

                        popScope();
                        if (AddedClassScope) {
                            popScope();
                        }

                        // Restore hidden class scopes in original order.
                        while (!HiddenClassScopes.empty()) {
                            Scopes_.push_back(std::move(HiddenClassScopes.back()));
                            HiddenClassScopes.pop_back();
                        }
                        return true;
                    }

                    void addClassMembers(const CXXRecordDecl* Record) {
                        if (!Record || !Record->hasDefinition()) {
                            return;
                        }

                        const CXXRecordDecl* Definition = Record->getDefinition();
                        for (const FieldDecl* Field : Definition->fields()) {
                            if (Field->getIdentifier()) {
                                Scopes_.back()[Field->getName()] = Field;
                            }
                        }

                        for (const Decl* Declaration : Definition->decls()) {
                            if (const auto* Enum = dyn_cast<EnumDecl>(Declaration)) {
                                if (!Enum->isScoped()) {
                                    for (const EnumConstantDecl* Enumerator : Enum->enumerators()) {
                                        if (Enumerator->getIdentifier()) {
                                            Scopes_.back()[Enumerator->getName()] = Enumerator;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    void addBaseClassMembers(const CXXRecordDecl* Record) {
                        for (const CXXBaseSpecifier& Base : Record->bases()) {
                            const Type* BaseType = Base.getType().getTypePtrOrNull();
                            if (!BaseType) {
                                continue;
                            }

                            const auto* BaseRecord = BaseType->getAsCXXRecordDecl();
                            if (!BaseRecord || !BaseRecord->hasDefinition()) {
                                continue;
                            }

                            addBaseClassMembers(BaseRecord->getDefinition());
                            addClassMembers(BaseRecord->getDefinition());
                        }
                    }

                    void handleDeclaration(const NamedDecl* Declaration) {
                        if (!Declaration || !Declaration->getIdentifier() || Declaration->isImplicit() ||
                            SourceMgr_.isInSystemHeader(Declaration->getLocation())) {
                            return;
                        }

                        StringRef Name = Declaration->getName();
                        for (auto Scope = Scopes_.rbegin() + 1; Scope != Scopes_.rend(); ++Scope) {
                            auto Found = Scope->find(Name);
                            if (Found == Scope->end()) {
                                continue;
                            }

                            Check_.diag(Declaration->getLocation(),
                                        "variable declaration hides a variable declared in an outer scope");
                            Check_.diag(Found->second->getLocation(), "hidden declaration is here",
                                        DiagnosticIDs::Note);
                            break;
                        }

                        Scopes_.back()[Name] = Declaration;
                    }

                    SdcNoVariableShadowingCheck& Check_;
                    ASTContext& Context_;
                    const SourceManager& SourceMgr_;
                    SmallVector<llvm::StringMap<const NamedDecl*>, 16> Scopes_;
                    // Number of scopes pushed by TraverseCXXRecordDecl that are
                    // currently on the stack.  Used to hide class-context scopes
                    // when traversing non-member (friend/free) functions.
                    size_t ClassScopeDepth_ = 0;
                    const Stmt* FunctionBody_ = nullptr;
                };

            } // namespace

            SdcNoVariableShadowingCheck::SdcNoVariableShadowingCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoVariableShadowingCheck::registerMatchers(MatchFinder* Finder) {
                Finder->addMatcher(translationUnitDecl().bind("translationUnit"), this);
            }

            void SdcNoVariableShadowingCheck::check(const MatchFinder::MatchResult& Result) {
                const auto* TranslationUnit = Result.Nodes.getNodeAs<TranslationUnitDecl>("translationUnit");
                if (!TranslationUnit) {
                    return;
                }

                ShadowingVisitor Visitor(*this, *Result.Context);
                Visitor.TraverseDecl(const_cast<TranslationUnitDecl*>(TranslationUnit));
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
