#include "size_type_check.h"

#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {

namespace {

// 'int n' is spelled with a builtin type, while 'std::size_t n' and 'auto n' are sugar over it.
AST_MATCHER(VarDecl, isSpelledAsBuiltinInteger) {
    const QualType type = Node.getType();
    return isa<BuiltinType>(type.getTypePtr()) && type->isIntegerType() && !type->isBooleanType() &&
           !type->isAnyCharacterType();
}

}  // namespace

void SizeTypeCheck::registerMatchers(MatchFinder* finder) {
    const auto sizeCall = ignoringParenImpCasts(anyOf(
        cxxMemberCallExpr(callee(cxxMethodDecl(hasAnyName("size", "length")))),
        callExpr(callee(functionDecl(hasAnyName("::std::size", "::std::ssize"))))));

    finder->addMatcher(varDecl(isSpelledAsBuiltinInteger(), hasInitializer(sizeCall)).bind("variable"), this);

    const auto counter = varDecl(isSpelledAsBuiltinInteger()).bind("variable");
    finder->addMatcher(
        forStmt(
            hasLoopInit(declStmt(hasSingleDecl(counter))),
            hasCondition(binaryOperator(hasOperands(
                ignoringParenImpCasts(declRefExpr(to(varDecl(equalsBoundNode("variable"))))), sizeCall)))),
        this);
}

void SizeTypeCheck::check(const MatchFinder::MatchResult& result) {
    const auto* variable = result.Nodes.getNodeAs<VarDecl>("variable");
    diag(variable->getLocation(), "'%0' holds a size or an index; use std::size_t instead of %1")
        << variable->getName() << variable->getType();
}

}  // namespace clang::tidy::eats
