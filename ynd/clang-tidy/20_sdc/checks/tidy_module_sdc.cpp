#include "bridge_header.h"

#include "taxi_async_use_after_free_check.h"
#include "taxi_coroutine_unsafe_check.h"
#include "taxi_dangling_config_ref_check.h"

#include "ascii_compare_ignore_case_check.h"
#include "uneeded_temporary_string_check.h"
#include "usage_restriction_checks.h"
#include "using_namespace_in_header_check.h"
#include "util_tstring_methods.h"

#include "SdcBannedIdentifierNameCheck.h"
#include "SdcBitwiseOperandsUnsignedCheck.h"
#include "SdcConsistentTypeAliasCheck.h"
#include "SdcDynamicMemoryAutomaticCheck.h"
#include "SdcEscapeSequenceCheck.h"
#include "SdcFunctionToPointerContextCheck.h"
#include "SdcGetenvPointerConstQualifiedCheck.h"
#include "SdcIdentifierLeadingUnderscoreCheck.h"
#include "SdcIntegerLiteralSuffixLongLongCheck.h"
#include "SdcIntegerLiteralSuffixUnsignedCheck.h"
#include "SdcNoAdvancedMemoryManagementCheck.h"
#include "SdcNoArithmeticCategoryChangeCheck.h"
#include "SdcNoArrayDecayInCallCheck.h"
#include "SdcNoAtoFunctionsCheck.h"
#include "SdcNoBoolConversionCheck.h"
#include "SdcNoCharacterConversionCheck.h"
#include "SdcNoCharacterFunctionsCheck.h"
#include "SdcNoCsetjmpFacilitiesCheck.h"
#include "SdcNoCsignalFacilitiesCheck.h"
#include "SdcNoCstdargFacilitiesCheck.h"
#include "SdcNoGlobalVariablesCheck.h"
#include "SdcNoImplicitThisCaptureCheck.h"
#include "SdcNoIoFunctionsCheck.h"
#include "SdcNoLowercaseLSuffixCheck.h"
#include "SdcNoMemFunctionsCheck.h"
#include "SdcNoOctalConstantsCheck.h"
#include "SdcNoSetlocaleGlobalCheck.h"
#include "SdcNoStackAddressAssignCheck.h"
#include "SdcNoStaticLocalVariablesCheck.h"
#include "SdcNoStringFunctionsCheck.h"
#include "SdcNoSystemCheck.h"
#include "SdcNoThrowStackAddressCheck.h"
#include "SdcNumericAssignmentCheck.h"
#include "SdcReservedNamespaceDefinitionCheck.h"
#include "SdcReturnValueUsedCheck.h"
#include "SdcSpecialMemberFunctionsCheck.h"
#include "SdcStdMoveNonConstLvalueCheck.h"
#include "SdcStringLiteralConcatenationCheck.h"
#include "SdcUnreachableCodeCheck.h"
#include "SdcUseNullptrCheck.h"
#include "SdcVirtualBaseCastViaDynamicCheck.h"
#include "SdcNoOffsetofCheck.h"
#include "SdcBannedMainCheck.h"
#include "SdcNoReinterpretCastCheck.h"
#include "SdcBlockScopeFunctionDeclarationCheck.h"
#include "SdcBlockScopeRedundantParenthesesCheck.h"
#include "SdcNoVariableShadowingCheck.h"
#include "SdcNoInheritedFunctionConcealingCheck.h"
#include "SdcNoDependentBaseUnqualifiedLookupCheck.h"
#include "SdcNoCStyleFunctionalCastsCheck.h"
#include "SdcNoCvQualificationRemovalCastCheck.h"
#include "SdcNoFunctionPointerCastsCheck.h"
#include "SdcNoIntEnumVoidPtrToPointerCastCheck.h"
#include "SdcNoObjectPointerToIntegralCastCheck.h"
#include "SdcNoAsmCheck.h"
#include "SdcNoUnionCheck.h"
#include "SdcNoLogicalOperatorOverloadCheck.h"
#include "SdcNoAddressofOperatorOverloadCheck.h"
#include "SdcNoFunctionTemplateSpecializationCheck.h"
#include "SdcEnumExplicitUnderlyingTypeCheck.h"
#include "SdcUnscopedEnumNumericUseCheck.h"
#include "SdcImplicitEnumConstantUniqueValueCheck.h"
#include "SdcBitFieldAppropriateTypeCheck.h"
#include "SdcOverrideDefaultArgCheck.h"
#include "SdcInitializerListOnlyConstructorCheck.h"
#include "SdcNoFunctionalCastStatementCheck.h"
#include "SdcIfElseIfFinalElseCheck.h"
#include "SdcSwitchStructureCheck.h"
#include "SdcForRangeInitOneCallCheck.h"
#include "SdcEmptyThrowInCatchCheck.h"
#include "SdcFuncTryBlockMemberRefCheck.h"
#include "SdcExceptionUnfriendlyNoexceptCheck.h"
#include "SdcNoTypeidPolymorphicCheck.h"
#include "SdcVirtualFinalNonOverrideCheck.h"
#include "SdcVirtualPMFNullCompareCheck.h"
#include "SdcExplicitSingleArgCtorCheck.h"


using namespace clang::ast_matchers;

namespace clang::tidy::arcadia {
class ArcadiaModule : public ClangTidyModule {
public:
    void addCheckFactories(ClangTidyCheckFactories& CheckFactories) override {
        CheckFactories.registerCheck<misc::TaxiCoroutineUnsafeCheck>("arcadia-taxi-coroutine-unsafe");
        CheckFactories.registerCheck<misc::TaxiDanglingConfigRefCheck>("arcadia-taxi-dangling-config-ref");
        CheckFactories.registerCheck<bugprone::TaxiAsyncUseAfterFreeCheck>("arcadia-taxi-async-use-after-free");

        // IGNF-1863
        CheckFactories.registerCheck<TypeidNameRestrictionCheck>("arcadia-typeid-name-restriction");
        CheckFactories.registerCheck<AsciiCompareIgnoreCaseCheck>("arcadia-ascii-compare-ignorecase");
        CheckFactories.registerCheck<UnneededTemporaryStringCheck>("arcadia-unneeded-temporary-string");
        CheckFactories.registerCheck<UsingNamespaceInHeaderCheck>("arcadia-using-namespace-in-header");
        CheckFactories.registerCheck<UtilTStringUpperCaseMethodsCheck>("arcadia-util-tstring-methods");

        CheckFactories.registerCheck<sdc::SdcBannedIdentifierNameCheck>("sdc-banned-identifier-name");
        CheckFactories.registerCheck<sdc::SdcBannedMainCheck>("sdc-banned-main");
        CheckFactories.registerCheck<sdc::SdcBitwiseOperandsUnsignedCheck>("sdc-bitwise-operands-unsigned");
        CheckFactories.registerCheck<sdc::SdcBlockScopeFunctionDeclarationCheck>("sdc-block-scope-function-declaration");
        CheckFactories.registerCheck<sdc::SdcBlockScopeRedundantParenthesesCheck>("sdc-block-scope-redundant-parentheses");
        CheckFactories.registerCheck<sdc::SdcConsistentTypeAliasCheck>("sdc-consistent-type-alias");
        CheckFactories.registerCheck<sdc::SdcDynamicMemoryAutomaticCheck>("sdc-dynamic-memory-automatic");
        CheckFactories.registerCheck<sdc::SdcEscapeSequenceCheck>("sdc-escape-sequence");
        CheckFactories.registerCheck<sdc::SdcFunctionToPointerContextCheck>("sdc-function-to-pointer-context");
        CheckFactories.registerCheck<sdc::SdcGetenvPointerConstQualifiedCheck>("sdc-getenv-pointer-const-qualified");
        CheckFactories.registerCheck<sdc::SdcIdentifierLeadingUnderscoreCheck>("sdc-identifier-leading-underscore");
        CheckFactories.registerCheck<sdc::SdcIntegerLiteralSuffixLongLongCheck>("sdc-integer-literal-suffix-longlong");
        CheckFactories.registerCheck<sdc::SdcIntegerLiteralSuffixUnsignedCheck>("sdc-integer-literal-suffix-unsigned");
        CheckFactories.registerCheck<sdc::SdcNoAdvancedMemoryManagementCheck>("sdc-no-advanced-memory-management");
        CheckFactories.registerCheck<sdc::SdcNoArithmeticCategoryChangeCheck>("sdc-no-arithmetic-category-change");
        CheckFactories.registerCheck<sdc::SdcNoArrayDecayInCallCheck>("sdc-no-array-decay-in-call");
        CheckFactories.registerCheck<sdc::SdcNoAtoFunctionsCheck>("sdc-no-ato-functions");
        CheckFactories.registerCheck<sdc::SdcNoBoolConversionCheck>("sdc-no-bool-conversion");
        CheckFactories.registerCheck<sdc::SdcNoCStyleFunctionalCastsCheck>("sdc-no-cstyle-functional-casts");
        CheckFactories.registerCheck<sdc::SdcNoCharacterConversionCheck>("sdc-no-character-conversion");
        CheckFactories.registerCheck<sdc::SdcNoCharacterFunctionsCheck>("sdc-no-character-functions");
        CheckFactories.registerCheck<sdc::SdcNoCsetjmpFacilitiesCheck>("sdc-no-csetjmp-facilities");
        CheckFactories.registerCheck<sdc::SdcNoCsignalFacilitiesCheck>("sdc-no-csignal-facilities");
        CheckFactories.registerCheck<sdc::SdcNoCstdargFacilitiesCheck>("sdc-no-cstdarg-facilities");
        CheckFactories.registerCheck<sdc::SdcNoCvQualificationRemovalCastCheck>("sdc-no-cv-qualification-removal-cast");
        CheckFactories.registerCheck<sdc::SdcNoDependentBaseUnqualifiedLookupCheck>("sdc-no-dependent-base-unqualified-lookup");
        CheckFactories.registerCheck<sdc::SdcNoFunctionPointerCastsCheck>("sdc-no-function-pointer-casts");
        CheckFactories.registerCheck<sdc::SdcNoGlobalVariablesCheck>("sdc-no-global-variables");
        CheckFactories.registerCheck<sdc::SdcNoImplicitThisCaptureCheck>("sdc-no-implicit-this-capture");
        CheckFactories.registerCheck<sdc::SdcNoInheritedFunctionConcealingCheck>("sdc-no-inherited-function-concealing");
        CheckFactories.registerCheck<sdc::SdcNoIntEnumVoidPtrToPointerCastCheck>("sdc-no-int-enum-voidptr-to-pointer-cast");
        CheckFactories.registerCheck<sdc::SdcNoIoFunctionsCheck>("sdc-no-io-functions");
        CheckFactories.registerCheck<sdc::SdcNoLowercaseLSuffixCheck>("sdc-no-lowercase-l-suffix");
        CheckFactories.registerCheck<sdc::SdcNoMemFunctionsCheck>("sdc-no-mem-functions");
        CheckFactories.registerCheck<sdc::SdcNoObjectPointerToIntegralCastCheck>("sdc-no-object-pointer-to-integral-cast");
        CheckFactories.registerCheck<sdc::SdcNoOctalConstantsCheck>("sdc-no-octal-constants");
        CheckFactories.registerCheck<sdc::SdcNoOffsetofCheck>("sdc-no-offsetof");
        CheckFactories.registerCheck<sdc::SdcNoReinterpretCastCheck>("sdc-no-reinterpret-cast");
        CheckFactories.registerCheck<sdc::SdcNoSetlocaleGlobalCheck>("sdc-no-setlocale-global");
        CheckFactories.registerCheck<sdc::SdcNoStackAddressAssignCheck>("sdc-no-stack-address-assign");
        CheckFactories.registerCheck<sdc::SdcNoStaticLocalVariablesCheck>("sdc-no-static-local-variables");
        CheckFactories.registerCheck<sdc::SdcNoStringFunctionsCheck>("sdc-no-string-functions");
        CheckFactories.registerCheck<sdc::SdcNoSystemCheck>("sdc-no-system");
        CheckFactories.registerCheck<sdc::SdcNoThrowStackAddressCheck>("sdc-no-throw-stack-address");
        CheckFactories.registerCheck<sdc::SdcNoVariableShadowingCheck>("sdc-no-variable-shadowing");
        CheckFactories.registerCheck<sdc::SdcNumericAssignmentCheck>("sdc-numeric-assignment");
        CheckFactories.registerCheck<sdc::SdcReservedNamespaceDefinitionCheck>("sdc-reserved-namespace-definition");
        CheckFactories.registerCheck<sdc::SdcReturnValueUsedCheck>("sdc-return-value-used");
        CheckFactories.registerCheck<sdc::SdcSpecialMemberFunctionsCheck>("sdc-special-member-functions");
        CheckFactories.registerCheck<sdc::SdcStdMoveNonConstLvalueCheck>("sdc-std-move-non-const-lvalue");
        CheckFactories.registerCheck<sdc::SdcStringLiteralConcatenationCheck>("sdc-string-literal-concatenation");
        CheckFactories.registerCheck<sdc::SdcUnreachableCodeCheck>("sdc-unreachable-code");
        CheckFactories.registerCheck<sdc::SdcUseNullptrCheck>("sdc-use-nullptr");
        CheckFactories.registerCheck<sdc::SdcVirtualBaseCastViaDynamicCheck>("sdc-virtual-base-cast-via-dynamic");
        CheckFactories.registerCheck<sdc::SdcNoAsmCheck>("sdc-no-asm");
        CheckFactories.registerCheck<sdc::SdcNoUnionCheck>("sdc-no-union");
        CheckFactories.registerCheck<sdc::SdcNoLogicalOperatorOverloadCheck>("sdc-no-logical-operator-overload");
        CheckFactories.registerCheck<sdc::SdcNoAddressofOperatorOverloadCheck>("sdc-no-addressof-operator-overload");
        CheckFactories.registerCheck<sdc::SdcNoFunctionTemplateSpecializationCheck>("sdc-no-function-template-specialization");
        CheckFactories.registerCheck<sdc::SdcEnumExplicitUnderlyingTypeCheck>("sdc-enum-explicit-underlying-type");
        CheckFactories.registerCheck<sdc::SdcUnscopedEnumNumericUseCheck>("sdc-no-unscoped-enum-numeric-use");
        CheckFactories.registerCheck<sdc::SdcImplicitEnumConstantUniqueValueCheck>("sdc-implicit-enum-constant-unique-value");
        CheckFactories.registerCheck<sdc::SdcBitFieldAppropriateTypeCheck>("sdc-bit-field-appropriate-type");
        CheckFactories.registerCheck<sdc::SdcOverrideDefaultArgCheck>("sdc-override-default-arg");
        CheckFactories.registerCheck<sdc::SdcInitializerListOnlyConstructorCheck>("sdc-initializer-list-only-constructor");
        CheckFactories.registerCheck<sdc::SdcNoFunctionalCastStatementCheck>("sdc-no-functional-cast-statement");
        CheckFactories.registerCheck<sdc::SdcIfElseIfFinalElseCheck>("sdc-if-else-if-final-else");
        CheckFactories.registerCheck<sdc::SdcSwitchStructureCheck>("sdc-switch-structure");
        CheckFactories.registerCheck<sdc::SdcForRangeInitOneCallCheck>("sdc-for-range-init-one-call");
        CheckFactories.registerCheck<sdc::SdcEmptyThrowInCatchCheck>("sdc-empty-throw-in-catch");
        CheckFactories.registerCheck<sdc::SdcFuncTryBlockMemberRefCheck>("sdc-func-try-block-member-ref");
        CheckFactories.registerCheck<sdc::SdcExceptionUnfriendlyNoexceptCheck>("sdc-exception-unfriendly-noexcept");
        CheckFactories.registerCheck<sdc::SdcNoTypeidPolymorphicCheck>("sdc-no-typeid-polymorphic");
        CheckFactories.registerCheck<sdc::SdcVirtualFinalNonOverrideCheck>("sdc-virtual-final-non-override");
        CheckFactories.registerCheck<sdc::SdcVirtualPMFNullCompareCheck>("sdc-virtual-pmf-null-compare");
        CheckFactories.registerCheck<sdc::SdcExplicitSingleArgCtorCheck>("sdc-explicit-single-arg-ctor");
    }
};

// Register the ArcadiaTidyModule using this statically initialized variable.
static ClangTidyModuleRegistry::Add<ArcadiaModule> X("arcadia-module", "Adds Arcadia specific lint checks.");

}  // namespace clang::tidy::arcadia

namespace clang::tidy {
// This anchor is used to force the linker to link in the generated object file
// and thus register the YandexModule.
volatile int YandexModuleAnchorSource = 0;
}  // namespace clang::tidy
