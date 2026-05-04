#pragma once

#include "bridge_header.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            // The pointers returned by the C++ Standard Library functions
            // localeconv, getenv, setlocale or strerror must only be used as if they have
            // pointer to const-qualified type.
            class SdcGetenvPointerConstQualifiedCheck: public ClangTidyCheck {
            public:
                SdcGetenvPointerConstQualifiedCheck(StringRef Name, ClangTidyContext* Context);
                void registerMatchers(ast_matchers::MatchFinder* Finder) override;
                void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

            private:
                void checkFunctionCall(const clang::CallExpr* Call,
                                       const ast_matchers::MatchFinder::MatchResult& Result);
                void checkVariableDeclaration(const clang::DeclStmt* DeclStmt,
                                              const ast_matchers::MatchFinder::MatchResult& Result);
                void checkVariableTypeAndUsage(const clang::VarDecl* VarDecl,
                                               const ast_matchers::MatchFinder::MatchResult& Result);
                void checkForModificationUsage(const clang::Stmt* SensitiveStmt,
                                               const clang::Stmt* Parent,
                                               const ast_matchers::MatchFinder::MatchResult& Result);
                void checkFunctionParameterUsage(const clang::CallExpr* SensitiveCall,
                                                 const clang::Stmt* Parent,
                                                 const ast_matchers::MatchFinder::MatchResult& Result);
                void checkCallParameterConstQualification(const clang::CallExpr* SensitiveCall,
                                                          const clang::CallExpr* OuterCall,
                                                          const ast_matchers::MatchFinder::MatchResult& Result);
                void checkDereferenceModification(const clang::UnaryOperator* UnaryOp,
                                                  const ast_matchers::MatchFinder::MatchResult& Result);
                void checkArraySubscriptModification(const clang::ArraySubscriptExpr* ArraySubscript,
                                                     const ast_matchers::MatchFinder::MatchResult& Result);
                void checkMemberAccessModification(const clang::MemberExpr* MemberExpr,
                                                   const ast_matchers::MatchFinder::MatchResult& Result);
                void checkBinaryOperationModification(const clang::BinaryOperator* BinaryOp,
                                                      const ast_matchers::MatchFinder::MatchResult& Result);

                const clang::Stmt* getParentIgnoreParensAndCasts(const clang::Stmt* S,
                                                                 clang::ASTContext* Context);
                bool hasSensitiveStmtInSubtree(const clang::Stmt* Root, const clang::Stmt* Target);
                llvm::StringRef getFunctionNameFromInitializer(const clang::Expr* Init);
                llvm::StringRef getFunctionNameFromCall(const clang::CallExpr* Call);
                clang::FixItHint getParameterLocationNote(const clang::ParmVarDecl* Param, int ParamIndex);

                // Data-flow analysis for lconv struct pointer fields
                bool isLconvPointerField(const clang::FieldDecl* Field);
                bool isMemberExprFromLocaleconv(const clang::MemberExpr* MemberExpr,
                                                const ast_matchers::MatchFinder::MatchResult& Result);
                void checkLconvMemberModification(const clang::MemberExpr* MemberExpr,
                                                  const ast_matchers::MatchFinder::MatchResult& Result);
                const clang::Expr* findSensitiveOrigin(const clang::Expr* E,
                                                       const ast_matchers::MatchFinder::MatchResult& Result);
                bool isFromLocaleconvCall(const clang::Expr* E,
                                          const ast_matchers::MatchFinder::MatchResult& Result);
            };

        } // namespace sdc
    } // namespace tidy
} // namespace clang
