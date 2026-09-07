#include "insurance_optional_usage_check.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/ExprCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/SourceManager.h>

using namespace clang::ast_matchers;

namespace clang::tidy::arcadia {
    namespace {

        bool IsInStdNamespace(const Decl* declaration) {
            for (const DeclContext* context = declaration->getDeclContext(); context;
                 context = context->getParent()) {
                const auto* namespaceDecl = dyn_cast<NamespaceDecl>(context);
                if (namespaceDecl && namespaceDecl->isStdNamespace()) {
                    return true;
                }
            }
            return false;
        }

        QualType GetCanonicalObjectType(QualType type) {
            return type.getNonReferenceType().getUnqualifiedType().getCanonicalType();
        }

        bool IsStdOptionalType(QualType type) {
            if (type.isNull()) {
                return false;
            }

            const auto* record = GetCanonicalObjectType(type)->getAsCXXRecordDecl();
            const auto* specialization = dyn_cast_or_null<ClassTemplateSpecializationDecl>(record);
            if (!specialization) {
                return false;
            }

            const ClassTemplateDecl* classTemplate = specialization->getSpecializedTemplate();
            return classTemplate->getName() == "optional" && IsInStdNamespace(classTemplate);
        }

        bool IsStdNulloptType(QualType type) {
            if (type.isNull()) {
                return false;
            }

            const auto* record = GetCanonicalObjectType(type)->getAsCXXRecordDecl();
            return record && record->getName() == "nullopt_t" && IsInStdNamespace(record);
        }

        SourceLocation GetUserWrittenLocation(SourceLocation location, const SourceManager& sourceManager) {
            if (!location.isValid()) {
                return {};
            }

            // A token passed as a macro argument is still written at the call site.
            // A token originating in a macro body is an implementation detail of that macro.
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

        SourceLocation GetMemberLocation(const CXXMemberCallExpr& call) {
            const Expr* callee = call.getCallee()->IgnoreParenImpCasts();
            if (const auto* member = dyn_cast<MemberExpr>(callee)) {
                return member->getMemberLoc();
            }
            return call.getExprLoc();
        }

        bool IsTransparentImplicitConversionWrapper(const Expr& expression) {
            return isa<ImplicitCastExpr, ParenExpr, ExprWithCleanups, MaterializeTemporaryExpr,
                       CXXBindTemporaryExpr>(expression);
        }

        SourceLocation GetImplicitConversionLocation(
            const CXXMemberCallExpr& call,
            ASTContext& context) {
            const Stmt* current = &call;
            while (true) {
                const auto parents = context.getParents(*current);
                if (parents.size() != 1) {
                    break;
                }

                if (const auto* parentExpression = parents[0].get<Expr>()) {
                    if (IsTransparentImplicitConversionWrapper(*parentExpression)) {
                        current = parentExpression;
                        continue;
                    }
                    return parentExpression->getExprLoc();
                }
                if (const auto* ifStatement = parents[0].get<IfStmt>()) {
                    return ifStatement->getIfLoc();
                }
                if (const auto* whileStatement = parents[0].get<WhileStmt>()) {
                    return whileStatement->getWhileLoc();
                }
                if (const auto* doStatement = parents[0].get<DoStmt>()) {
                    return doStatement->getWhileLoc();
                }
                if (const auto* forStatement = parents[0].get<ForStmt>()) {
                    return forStatement->getForLoc();
                }
                break;
            }

            return call.getBeginLoc();
        }

        const Expr* IgnoreParenImplicitCasts(const Expr* expression) {
            return expression ? expression->IgnoreParenImpCasts() : nullptr;
        }

    } // namespace

    void InsuranceOptionalUsageCheck::registerMatchers(MatchFinder* finder) {
        finder->addMatcher(
            cxxOperatorCallExpr(
                hasAnyOverloadedOperatorName("*", "->", "==", "!="),
                unless(isInTemplateInstantiation()),
                unless(isExpansionInSystemHeader()))
                .bind("optionalOperatorCall"),
            this);

        finder->addMatcher(
            cxxMemberCallExpr(
                callee(cxxMethodDecl(anyOf(
                    hasName("value"),
                    hasAnyOverloadedOperatorName("*", "->")))),
                unless(isInTemplateInstantiation()),
                unless(isExpansionInSystemHeader()))
                .bind("optionalMemberCall"),
            this);

        finder->addMatcher(
            cxxMemberCallExpr(
                callee(cxxConversionDecl()),
                unless(isInTemplateInstantiation()),
                unless(isExpansionInSystemHeader()))
                .bind("optionalConversionCall"),
            this);
    }

    void InsuranceOptionalUsageCheck::check(const MatchFinder::MatchResult& result) {
        const SourceManager& sourceManager = *result.SourceManager;

        if (const auto* call = result.Nodes.getNodeAs<CXXOperatorCallExpr>("optionalOperatorCall")) {
            const OverloadedOperatorKind operatorKind = call->getOperator();
            if (operatorKind == OO_Star || operatorKind == OO_Arrow) {
                if (call->getNumArgs() == 0 || !IsStdOptionalType(call->getArg(0)->getType())) {
                    return;
                }

                const SourceLocation location =
                    GetUserWrittenLocation(call->getOperatorLoc(), sourceManager);
                if (!location.isValid()) {
                    return;
                }

                if (operatorKind == OO_Star) {
                    diag(location,
                         "do not access std::optional with operator*; use "
                         "optional_value::Value() instead");
                } else {
                    diag(location,
                         "do not access std::optional with operator->; use "
                         "optional_value::Value() instead");
                }
                return;
            }

            if ((operatorKind == OO_EqualEqual || operatorKind == OO_ExclaimEqual) &&
                call->getNumArgs() == 2) {
                const Expr* lhs = IgnoreParenImplicitCasts(call->getArg(0));
                const Expr* rhs = IgnoreParenImplicitCasts(call->getArg(1));
                const bool comparesWithNullopt =
                    (IsStdOptionalType(lhs->getType()) && IsStdNulloptType(rhs->getType())) ||
                    (IsStdNulloptType(lhs->getType()) && IsStdOptionalType(rhs->getType()));
                if (!comparesWithNullopt) {
                    return;
                }

                const SourceLocation location =
                    GetUserWrittenLocation(call->getOperatorLoc(), sourceManager);
                if (location.isValid()) {
                    diag(location,
                         "check std::optional presence with has_value() instead of comparing "
                         "with std::nullopt");
                }
            }
            return;
        }

        if (const auto* call = result.Nodes.getNodeAs<CXXMemberCallExpr>("optionalMemberCall")) {
            if (!IsStdOptionalType(call->getObjectType())) {
                return;
            }

            const CXXMethodDecl* method = call->getMethodDecl();
            const SourceLocation location =
                GetUserWrittenLocation(GetMemberLocation(*call), sourceManager);
            if (!method || !location.isValid()) {
                return;
            }

            if (method->getName() == "value") {
                diag(location,
                     "do not access std::optional with value(); use optional_value::Value() "
                     "instead");
            } else if (method->getOverloadedOperator() == OO_Star) {
                diag(location,
                     "do not access std::optional with operator*; use optional_value::Value() "
                     "instead");
            } else if (method->getOverloadedOperator() == OO_Arrow) {
                diag(location,
                     "do not access std::optional with operator->; use optional_value::Value() "
                     "instead");
            }
            return;
        }

        const auto* call = result.Nodes.getNodeAs<CXXMemberCallExpr>("optionalConversionCall");
        if (!call || !IsStdOptionalType(call->getObjectType())) {
            return;
        }

        const auto* conversion = dyn_cast_or_null<CXXConversionDecl>(call->getMethodDecl());
        if (!conversion || !conversion->getConversionType()->isBooleanType()) {
            return;
        }

        SourceLocation location = GetMemberLocation(*call);
        if (!location.isValid()) {
            location = GetImplicitConversionLocation(*call, *result.Context);
        }
        location = GetUserWrittenLocation(location, sourceManager);
        if (location.isValid()) {
            diag(location, "check std::optional presence with has_value()");
        }
    }

} // namespace clang::tidy::arcadia
