#include "complex_condition_check.h"

#include <clang/ASTMatchers/ASTMatchFinder.h>

using namespace clang::ast_matchers;

namespace clang::tidy::eats {
    namespace {

        unsigned CountOperands(const Expr* condition_ptr) {
            condition_ptr = condition_ptr->IgnoreParenImpCasts();
            if (const auto* binary_ptr = dyn_cast<BinaryOperator>(condition_ptr); binary_ptr && binary_ptr->isLogicalOp()) {
                return CountOperands(binary_ptr->getLHS()) + CountOperands(binary_ptr->getRHS());
            }
            if (const auto* unary_ptr = dyn_cast<UnaryOperator>(condition_ptr); unary_ptr && unary_ptr->getOpcode() == UO_LNot) {
                return CountOperands(unary_ptr->getSubExpr());
            }
            return 1;
        }

    } // namespace

    void ComplexConditionCheck::storeOptions(ClangTidyOptions::OptionMap& options) {
        Options.store(options, "MaxOperands", maxOperands_);
    }

    void ComplexConditionCheck::registerMatchers(MatchFinder* finder_ptr) {
        finder_ptr->addMatcher(ifStmt(hasCondition(expr().bind("condition"))), this);
        finder_ptr->addMatcher(whileStmt(hasCondition(expr().bind("condition"))), this);
    }

    void ComplexConditionCheck::check(const MatchFinder::MatchResult& result) {
        const auto* condition_ptr = result.Nodes.getNodeAs<Expr>("condition");
        if (condition_ptr->getBeginLoc().isMacroID()) {
            return;
        }
        const unsigned operands = CountOperands(condition_ptr);
        if (operands > maxOperands_) {
            diag(condition_ptr->getBeginLoc(), "condition has %0 operands (maximum %1); extract a named predicate")
                << operands << maxOperands_;
        }
    }

} // namespace clang::tidy::eats
