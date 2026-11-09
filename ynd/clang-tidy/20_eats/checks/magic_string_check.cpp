#include "magic_string_check.h"

#include <clang/AST/ExprCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {
    namespace {

        const StringLiteral* GetStringLiteral(const Expr* operand_ptr) {
            operand_ptr = operand_ptr->IgnoreParenImpCasts();
            if (const auto* cleanup_ptr = dyn_cast<ExprWithCleanups>(operand_ptr)) {
                return GetStringLiteral(cleanup_ptr->getSubExpr());
            }
            if (const auto* temporary_ptr = dyn_cast<MaterializeTemporaryExpr>(operand_ptr)) {
                return GetStringLiteral(temporary_ptr->getSubExpr());
            }
            if (const auto* binding_ptr = dyn_cast<CXXBindTemporaryExpr>(operand_ptr)) {
                return GetStringLiteral(binding_ptr->getSubExpr());
            }
            if (const auto* cast_ptr = dyn_cast<CXXFunctionalCastExpr>(operand_ptr)) {
                return GetStringLiteral(cast_ptr->getSubExpr());
            }
            if (const auto* construction_ptr = dyn_cast<CXXConstructExpr>(operand_ptr);
                construction_ptr && construction_ptr->getNumArgs() != 0) {
                return GetStringLiteral(construction_ptr->getArg(0));
            }
            if (const auto* literal_ptr = dyn_cast<UserDefinedLiteral>(operand_ptr);
                literal_ptr && literal_ptr->getNumArgs() != 0) {
                return GetStringLiteral(literal_ptr->getArg(0));
            }
            return dyn_cast<StringLiteral>(operand_ptr);
        }

    } // namespace

    void MagicStringCheck::registerMatchers(MatchFinder* finder_ptr) {
        finder_ptr->addMatcher(binaryOperation(
                                   anyOf(hasOperatorName("=="), hasOperatorName("!=")),
                                   hasLHS(expr().bind("lhs")), hasRHS(expr().bind("rhs"))), this);
    }

    void MagicStringCheck::check(const MatchFinder::MatchResult& result) {
        const Expr* operands[] = {
            result.Nodes.getNodeAs<Expr>("lhs"),
            result.Nodes.getNodeAs<Expr>("rhs"),
        };
        for (const auto* operand_ptr : operands) {
            const auto* literal_ptr = GetStringLiteral(operand_ptr);
            if (!literal_ptr || literal_ptr->getLength() == 0 || literal_ptr->getBeginLoc().isMacroID()) {
                continue;
            }
            diag(literal_ptr->getBeginLoc(), "string literal in a comparison is a magic value; use a named constant");
        }
    }

} // namespace clang::tidy::eats
