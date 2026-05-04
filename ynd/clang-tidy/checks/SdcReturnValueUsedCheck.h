#pragma once

#include "bridge_header.h"
#include <set>

namespace clang {
namespace tidy {
namespace sdc {

class SdcReturnValueUsedCheck : public ClangTidyCheck {
public:
    SdcReturnValueUsedCheck(StringRef Name, ClangTidyContext* Context);

    void registerMatchers(ast_matchers::MatchFinder* Finder) override;
    void check(const ast_matchers::MatchFinder::MatchResult& Result) override;

private:
    void checkUnusedFunctionCall(const CallExpr* Call,
                                 const ast_matchers::MatchFinder::MatchResult& Result);

    void checkUnusedLambdaCall(const CXXOperatorCallExpr* OperatorCall,
                               const ast_matchers::MatchFinder::MatchResult& Result);

    void checkUnusedMemberCall(const CXXMemberCallExpr* MemberCall,
                               const ast_matchers::MatchFinder::MatchResult& Result);

    bool isReturnValueUsed(const Expr* CallExpr,
                           const ast_matchers::MatchFinder::MatchResult& Result);

    bool containsExpression(const Stmt* Container, const Expr* Target);

    bool isDiscardedCorrectly(const Stmt* Parent);

    const Stmt* getParentIgnoreParensAndCasts(const Stmt* S, ASTContext* Context);

    std::set<const CallExpr*> CtorInitCalls;
};

} // namespace sdc
} // namespace tidy
} // namespace clang
