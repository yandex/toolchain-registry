#include "insurance_enum_usage_check.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/SourceManager.h>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>

using namespace clang::ast_matchers;

namespace clang::tidy::arcadia {
    namespace {

        SourceLocation GetUserWrittenLocation(SourceLocation location, const SourceManager& sourceManager) {
            if (!location.isValid()) {
                return {};
            }

            while (location.isMacroID()) {
                if (!sourceManager.isMacroArgExpansion(location)) {
                    return {};
                }
                location = sourceManager.getImmediateSpellingLoc(location);
            }

            if (!location.isValid() || sourceManager.isInSystemHeader(location)) {
                return {};
            }
            return location;
        }

        const Expr* IgnoreParenImplicitCasts(const Expr* expression) {
            return expression ? expression->IgnoreParenImpCasts() : nullptr;
        }

        const EnumDecl* GetCanonicalEnumDecl(QualType type) {
            if (type.isNull()) {
                return nullptr;
            }

            type = type.getNonReferenceType().getUnqualifiedType().getCanonicalType();
            const auto* enumType = type->getAs<EnumType>();
            return enumType ? enumType->getDecl()->getCanonicalDecl() : nullptr;
        }

        const EnumConstantDecl* GetDirectEnumConstant(const Expr* expression) {
            const auto* reference = dyn_cast_or_null<DeclRefExpr>(IgnoreParenImplicitCasts(expression));
            return reference ? dyn_cast<EnumConstantDecl>(reference->getDecl()) : nullptr;
        }

        SourceLocation GetComparisonOperatorLocation(const Expr& comparison) {
            if (const auto* binaryOperator = dyn_cast<BinaryOperator>(&comparison)) {
                return binaryOperator->getOperatorLoc();
            }
            if (const auto* operatorCall = dyn_cast<CXXOperatorCallExpr>(&comparison)) {
                return operatorCall->getOperatorLoc();
            }
            if (const auto* rewrittenOperator = dyn_cast<CXXRewrittenBinaryOperator>(&comparison)) {
                return rewrittenOperator->getOperatorLoc();
            }
            return {};
        }

        bool IsInIfCondition(const Expr& comparison, ASTContext& context) {
            llvm::SmallVector<const Stmt*, 8> worklist;
            llvm::SmallPtrSet<const Stmt*, 16> visited;
            worklist.push_back(&comparison);
            visited.insert(&comparison);

            while (!worklist.empty()) {
                const Stmt* current = worklist.pop_back_val();
                for (const auto& parent : context.getParents(*current)) {
                    const auto* parentStatement = parent.get<Stmt>();
                    if (!parentStatement) {
                        continue;
                    }

                    if (const auto* ifStatement = dyn_cast<IfStmt>(parentStatement)) {
                        if (ifStatement->getCond() == current) {
                            return true;
                        }
                    }

                    if (visited.insert(parentStatement).second) {
                        worklist.push_back(parentStatement);
                    }
                }
            }

            return false;
        }

    } // namespace

    void InsuranceEnumUsageCheck::registerMatchers(MatchFinder* finder) {
        finder->addMatcher(
            enumDecl(
                unless(isScoped()),
                unless(isImplicit()),
                unless(isInstantiated()),
                unless(isExpansionInSystemHeader()))
                .bind("unscopedEnum"),
            this);

        finder->addMatcher(
            switchStmt(
                unless(isInTemplateInstantiation()),
                unless(isExpansionInSystemHeader()))
                .bind("enumSwitch"),
            this);

        // A rewritten operator also contains its semantic comparison. Match that child
        // and skip the wrapper to avoid reporting the same source operator twice.
        finder->addMatcher(
            binaryOperation(
                hasAnyOperatorName("==", "!="),
                hasLHS(expr().bind("enumComparisonLhs")),
                hasRHS(expr().bind("enumComparisonRhs")),
                unless(isInTemplateInstantiation()),
                unless(cxxRewrittenBinaryOperator()),
                unless(isExpansionInSystemHeader()))
                .bind("enumComparison"),
            this);
    }

    void InsuranceEnumUsageCheck::check(const MatchFinder::MatchResult& result) {
        const SourceManager& sourceManager = *result.SourceManager;

        if (const auto* declaration = result.Nodes.getNodeAs<EnumDecl>("unscopedEnum")) {
            if (declaration != declaration->getCanonicalDecl()) {
                return;
            }

            const SourceLocation location =
                GetUserWrittenLocation(declaration->getBeginLoc(), sourceManager);
            if (location.isValid()) {
                diag(location, "use enum class instead of an unscoped enum");
            }
            return;
        }

        if (const auto* switchStatement = result.Nodes.getNodeAs<SwitchStmt>("enumSwitch")) {
            const Expr* condition = IgnoreParenImplicitCasts(switchStatement->getCond());
            if (!condition || !GetCanonicalEnumDecl(condition->getType())) {
                return;
            }

            for (const SwitchCase* switchCase = switchStatement->getSwitchCaseList(); switchCase;
                 switchCase = switchCase->getNextSwitchCase()) {
                const auto* defaultStatement = dyn_cast<DefaultStmt>(switchCase);
                if (!defaultStatement) {
                    continue;
                }

                const SourceLocation location =
                    GetUserWrittenLocation(defaultStatement->getDefaultLoc(), sourceManager);
                if (location.isValid()) {
                    diag(location, "do not use a default label in a switch over an enum");
                }
            }
            return;
        }

        const Expr* comparison = nullptr;
        if (const auto* binaryOperator =
                result.Nodes.getNodeAs<BinaryOperator>("enumComparison")) {
            comparison = binaryOperator;
        } else if (const auto* operatorCall =
                       result.Nodes.getNodeAs<CXXOperatorCallExpr>("enumComparison")) {
            comparison = operatorCall;
        } else if (const auto* rewrittenOperator =
                       result.Nodes.getNodeAs<CXXRewrittenBinaryOperator>("enumComparison")) {
            comparison = rewrittenOperator;
        }
        if (!comparison) {
            return;
        }

        const Expr* lhs = IgnoreParenImplicitCasts(
            result.Nodes.getNodeAs<Expr>("enumComparisonLhs"));
        const Expr* rhs = IgnoreParenImplicitCasts(
            result.Nodes.getNodeAs<Expr>("enumComparisonRhs"));
        if (!lhs || !rhs) {
            return;
        }

        const EnumDecl* lhsEnum = GetCanonicalEnumDecl(lhs->getType());
        const EnumDecl* rhsEnum = GetCanonicalEnumDecl(rhs->getType());
        if (!lhsEnum || !rhsEnum) {
            return;
        }

        const bool hasDirectConstant =
            GetDirectEnumConstant(lhs) || GetDirectEnumConstant(rhs);
        if (!hasDirectConstant && !IsInIfCondition(*comparison, *result.Context)) {
            return;
        }

        const SourceLocation location = GetUserWrittenLocation(
            GetComparisonOperatorLocation(*comparison),
            sourceManager);
        if (location.isValid()) {
            diag(location, "do not compare enum values directly; use a switch statement");
        }
    }

} // namespace clang::tidy::arcadia
