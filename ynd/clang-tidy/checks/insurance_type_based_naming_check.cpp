#include "insurance_type_based_naming_check.h"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Type.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/SourceManager.h>

using namespace clang::ast_matchers;

namespace clang::tidy::arcadia {
    namespace {

        enum class NamingType {
            kOther,
            kOptional,
            kTaskWithResult,
        };

        QualType RemoveTopLevelCvAndReference(QualType type) {
            if (type.isNull()) {
                return {};
            }

            type = type.getCanonicalType();
            if (type->isReferenceType()) {
                type = type->getPointeeType().getCanonicalType();
            }
            return type.getUnqualifiedType();
        }

        const ClassTemplateDecl* GetClassTemplate(QualType type) {
            type = RemoveTopLevelCvAndReference(type);
            if (type.isNull() || type->isDependentType()) {
                return nullptr;
            }

            const auto* record_type = type->getAs<RecordType>();
            if (!record_type) {
                return nullptr;
            }

            const auto* specialization = dyn_cast<ClassTemplateSpecializationDecl>(record_type->getDecl());
            return specialization ? specialization->getSpecializedTemplate()->getCanonicalDecl() : nullptr;
        }

        bool IsInEngineNamespace(const ClassTemplateDecl& declaration) {
            const DeclContext* context = declaration.getDeclContext();
            while (context && !context->isTranslationUnit()) {
                if (const auto* namespace_declaration = dyn_cast<NamespaceDecl>(context)) {
                    if (namespace_declaration->getName() == "engine") {
                        return true;
                    }
                }
                context = context->getParent();
            }
            return false;
        }

        NamingType ClassifyType(QualType type) {
            const ClassTemplateDecl* declaration = GetClassTemplate(type);
            if (!declaration) {
                return NamingType::kOther;
            }

            if (declaration->getName() == "optional" && declaration->isInStdNamespace()) {
                return NamingType::kOptional;
            }
            if (declaration->getName() == "TaskWithResult" && IsInEngineNamespace(*declaration)) {
                return NamingType::kTaskWithResult;
            }
            return NamingType::kOther;
        }

        AST_MATCHER(QualType, isInsuranceNamingType) {
            return ClassifyType(Node) != NamingType::kOther;
        }

        bool IsCheckableLocation(const NamedDecl& declaration, const SourceManager& source_manager) {
            const SourceLocation location = declaration.getLocation();
            return location.isValid() &&
                   !source_manager.isInSystemHeader(location) &&
                   !source_manager.isInSystemMacro(location) &&
                   !source_manager.isMacroBodyExpansion(location);
        }

        bool IsLanguageDefinedFunctionName(const FunctionDecl& declaration) {
            return !declaration.getIdentifier() ||
                   declaration.isOverloadedOperator() ||
                   isa<CXXConstructorDecl>(declaration) ||
                   isa<CXXDestructorDecl>(declaration) ||
                   isa<CXXConversionDecl>(declaration) ||
                   isa<CXXDeductionGuideDecl>(declaration);
        }

        bool IsExcludedMethod(const FunctionDecl& declaration) {
            const auto* method = dyn_cast<CXXMethodDecl>(&declaration);
            return method && (method->getParent()->isLambda() || method->size_overridden_methods() != 0);
        }

        bool IsCanonicalParameterDeclaration(const ParmVarDecl& parameter) {
            const auto* function = dyn_cast<FunctionDecl>(parameter.getDeclContext());
            return !function || function->getCanonicalDecl() == function;
        }

    } // namespace

    void InsuranceTypeBasedNamingCheck::registerMatchers(MatchFinder* finder) {
        finder->addMatcher(
            functionDecl(
                unless(isImplicit()),
                unless(isInstantiated()),
                returns(isInsuranceNamingType()))
                .bind("function"),
            this);
        finder->addMatcher(
            varDecl(
                unless(isImplicit()),
                unless(isInstantiated()),
                hasType(isInsuranceNamingType()))
                .bind("variable"),
            this);
    }

    void InsuranceTypeBasedNamingCheck::check(const MatchFinder::MatchResult& result) {
        const SourceManager& source_manager = *result.SourceManager;

        if (const auto* function = result.Nodes.getNodeAs<FunctionDecl>("function")) {
            if (function->getCanonicalDecl() != function ||
                !IsCheckableLocation(*function, source_manager) ||
                IsLanguageDefinedFunctionName(*function) ||
                IsExcludedMethod(*function)) {
                return;
            }

            const StringRef name = function->getName();
            switch (ClassifyType(function->getReturnType())) {
                case NamingType::kOptional:
                    if (!name.starts_with("Opt")) {
                        diag(function->getLocation(), "function returning std::optional must have a name starting with 'Opt'");
                    }
                    break;
                case NamingType::kTaskWithResult:
                    if (isa<CXXMethodDecl>(function) && !name.starts_with("Async")) {
                        diag(
                            function->getLocation(),
                            "method returning engine::TaskWithResult must have a name starting with 'Async'");
                    }
                    break;
                case NamingType::kOther:
                    break;
            }
            return;
        }

        const auto* variable = result.Nodes.getNodeAs<VarDecl>("variable");
        if (!variable || !IsCheckableLocation(*variable, source_manager) || !variable->getIdentifier()) {
            return;
        }

        if (const auto* parameter = dyn_cast<ParmVarDecl>(variable)) {
            if (!IsCanonicalParameterDeclaration(*parameter)) {
                return;
            }
        } else if (!variable->hasLocalStorage()) {
            return;
        }

        const StringRef name = variable->getName();
        switch (ClassifyType(variable->getType())) {
            case NamingType::kOptional:
                if (!name.starts_with("maybe_")) {
                    diag(variable->getLocation(), "variable of type std::optional must have a name starting with 'maybe_'");
                }
                break;
            case NamingType::kTaskWithResult:
                if (!name.ends_with("_future")) {
                    diag(
                        variable->getLocation(),
                        "variable of type engine::TaskWithResult must have a name ending with '_future'");
                }
                break;
            case NamingType::kOther:
                break;
        }
    }

} // namespace clang::tidy::arcadia
