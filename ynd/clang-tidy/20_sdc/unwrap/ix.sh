{% extends '//clang/20/template.sh' %}

{% block bld_deps %}
{{super()}}
//clang/20
{% endblock %}

{% block llvm_targets %}
clang-tidy
clang-apply-replacements
clang-resource-headers
{% endblock %}

{% block check_srcs %}
bridge_header.h
ascii_compare_ignore_case_check.cpp
ascii_compare_ignore_case_check.h
taxi_async_use_after_free_check.cpp
taxi_async_use_after_free_check.h
taxi_coroutine_unsafe_check.cpp
taxi_coroutine_unsafe_check.h
taxi_dangling_config_ref_check.cpp
taxi_dangling_config_ref_check.h
uneeded_temporary_string_check.cpp
uneeded_temporary_string_check.h
usage_restriction_checks.cpp
usage_restriction_checks.h
using_namespace_in_header_check.cpp
using_namespace_in_header_check.h
util_tstring_methods.cpp
util_tstring_methods.h
possible_nullptr_check.cpp
{% endblock %}

{% block sdc_check_srcs %}
SdcAlgorithmResultUsedCheck.cpp
SdcAlgorithmResultUsedCheck.h
SdcAssertConstantCheck.cpp
SdcAssertConstantCheck.h
SdcBannedHeaderFacilitiesCheck.cpp
SdcBannedHeaderFacilitiesCheck.h
SdcBannedIdentifierNameCheck.cpp
SdcBannedIdentifierNameCheck.h
SdcBannedMainCheck.cpp
SdcBannedMainCheck.h
SdcBitFieldAppropriateTypeCheck.cpp
SdcBitFieldAppropriateTypeCheck.h
SdcBitwiseOperandsUnsignedCheck.cpp
SdcBitwiseOperandsUnsignedCheck.h
SdcBlockScopeFunctionDeclarationCheck.cpp
SdcBlockScopeFunctionDeclarationCheck.h
SdcBlockScopeRedundantParenthesesCheck.cpp
SdcBlockScopeRedundantParenthesesCheck.h
SdcCastUtils.cpp
SdcCastUtils.h
SdcConsistentTypeAliasCheck.cpp
SdcConsistentTypeAliasCheck.h
SdcDeleteIncompletePtrCheck.cpp
SdcDeleteIncompletePtrCheck.h
SdcDynamicMemoryAutomaticCheck.cpp
SdcDynamicMemoryAutomaticCheck.h
SdcEllipsisArgTypeCheck.cpp
SdcEllipsisArgTypeCheck.h
SdcEmptyThrowInCatchCheck.cpp
SdcEmptyThrowInCatchCheck.h
SdcEnumExplicitUnderlyingTypeCheck.cpp
SdcEnumExplicitUnderlyingTypeCheck.h
SdcErrnoZeroAssignCheck.cpp
SdcErrnoZeroAssignCheck.h
SdcEscapeSequenceCheck.cpp
SdcEscapeSequenceCheck.h
SdcExceptionUnfriendlyNoexceptCheck.cpp
SdcExceptionUnfriendlyNoexceptCheck.h
SdcExplicitSingleArgCtorCheck.cpp
SdcExplicitSingleArgCtorCheck.h
SdcForRangeInitOneCallCheck.cpp
SdcForRangeInitOneCallCheck.h
SdcForwardingReferenceCheck.cpp
SdcForwardingReferenceCheck.h
SdcFuncTryBlockMemberRefCheck.cpp
SdcFuncTryBlockMemberRefCheck.h
SdcFunctionToPointerContextCheck.cpp
SdcFunctionToPointerContextCheck.h
SdcGetenvPointerConstQualifiedCheck.cpp
SdcGetenvPointerConstQualifiedCheck.h
SdcIdentifierLeadingUnderscoreCheck.cpp
SdcIdentifierLeadingUnderscoreCheck.h
SdcIfElseIfFinalElseCheck.cpp
SdcIfElseIfFinalElseCheck.h
SdcIfEndifSameFileCheck.cpp
SdcIfEndifSameFileCheck.h
SdcImplicitEnumConstantUniqueValueCheck.cpp
SdcImplicitEnumConstantUniqueValueCheck.h
SdcIncludeFilenameCharsCheck.cpp
SdcIncludeFilenameCharsCheck.h
SdcIncludeFormatCheck.cpp
SdcIncludeFormatCheck.h
SdcInitializerListOnlyConstructorCheck.cpp
SdcInitializerListOnlyConstructorCheck.h
SdcIntegerLiteralSuffix.cpp
SdcIntegerLiteralSuffix.h
SdcIntegerLiteralSuffixLongLongCheck.cpp
SdcIntegerLiteralSuffixLongLongCheck.h
SdcIntegerLiteralSuffixUnsignedCheck.cpp
SdcIntegerLiteralSuffixUnsignedCheck.h
SdcInvalidDirectiveCheck.cpp
SdcInvalidDirectiveCheck.h
SdcMacroDirectiveInArgCheck.cpp
SdcMacroDirectiveInArgCheck.h
SdcMacroHashHashCheck.cpp
SdcMacroHashHashCheck.h
SdcMacroMixedUseCheck.cpp
SdcMacroMixedUseCheck.h
SdcMacroParensCheck.cpp
SdcMacroParensCheck.h
SdcMovedFromStateCheck.cpp
SdcMovedFromStateCheck.h
SdcNoAddressofOperatorOverloadCheck.cpp
SdcNoAddressofOperatorOverloadCheck.h
SdcNoAdvancedMemoryManagementCheck.cpp
SdcNoAdvancedMemoryManagementCheck.h
SdcNoArithmeticCategoryChangeCheck.cpp
SdcNoArithmeticCategoryChangeCheck.h
SdcNoArrayDecayInCallCheck.cpp
SdcNoArrayDecayInCallCheck.h
SdcNoAsmCheck.cpp
SdcNoAsmCheck.h
SdcNoAtoFunctionsCheck.cpp
SdcNoAtoFunctionsCheck.h
SdcNoBoolConversionCheck.cpp
SdcNoBoolConversionCheck.h
SdcNoCStyleFunctionalCastsCheck.cpp
SdcNoCStyleFunctionalCastsCheck.h
SdcNoCharacterConversionCheck.cpp
SdcNoCharacterConversionCheck.h
SdcNoCharacterFunctionsCheck.cpp
SdcNoCharacterFunctionsCheck.h
SdcNoCsetjmpFacilitiesCheck.cpp
SdcNoCsetjmpFacilitiesCheck.h
SdcNoCsignalFacilitiesCheck.cpp
SdcNoCsignalFacilitiesCheck.h
SdcNoCstdargFacilitiesCheck.cpp
SdcNoCstdargFacilitiesCheck.h
SdcNoCvQualificationRemovalCastCheck.cpp
SdcNoCvQualificationRemovalCastCheck.h
SdcNoDependentBaseUnqualifiedLookupCheck.cpp
SdcNoDependentBaseUnqualifiedLookupCheck.h
SdcNoFunctionLikeMacrosCheck.cpp
SdcNoFunctionLikeMacrosCheck.h
SdcNoFunctionPointerCastsCheck.cpp
SdcNoFunctionPointerCastsCheck.h
SdcNoFunctionTemplateSpecializationCheck.cpp
SdcNoFunctionTemplateSpecializationCheck.h
SdcNoFunctionalCastStatementCheck.cpp
SdcNoFunctionalCastStatementCheck.h
SdcNoGlobalVariablesCheck.cpp
SdcNoGlobalVariablesCheck.h
SdcNoImplicitThisCaptureCheck.cpp
SdcNoImplicitThisCaptureCheck.h
SdcNoInheritedFunctionConcealingCheck.cpp
SdcNoInheritedFunctionConcealingCheck.h
SdcNoIntEnumVoidPtrToPointerCastCheck.cpp
SdcNoIntEnumVoidPtrToPointerCastCheck.h
SdcNoIoFunctionsCheck.cpp
SdcNoIoFunctionsCheck.h
SdcNoLogicalOperatorOverloadCheck.cpp
SdcNoLogicalOperatorOverloadCheck.h
SdcNoLowercaseLSuffixCheck.cpp
SdcNoLowercaseLSuffixCheck.h
SdcNoMemFunctionsCheck.cpp
SdcNoMemFunctionsCheck.h
SdcNoObjectPointerToIntegralCastCheck.cpp
SdcNoObjectPointerToIntegralCastCheck.h
SdcNoOctalConstantsCheck.cpp
SdcNoOctalConstantsCheck.h
SdcNoOffsetofCheck.cpp
SdcNoOffsetofCheck.h
SdcNoReinterpretCastCheck.cpp
SdcNoReinterpretCastCheck.h
SdcNoReturnStackAddressCheck.cpp
SdcNoReturnStackAddressCheck.h
SdcNoSetlocaleGlobalCheck.cpp
SdcNoSetlocaleGlobalCheck.h
SdcNoStackAddressAssignCheck.cpp
SdcNoStackAddressAssignCheck.h
SdcNoStaticLocalVariablesCheck.cpp
SdcNoStaticLocalVariablesCheck.h
SdcNoStringFunctionsCheck.cpp
SdcNoStringFunctionsCheck.h
SdcNoSystemCheck.cpp
SdcNoSystemCheck.h
SdcNoThrowStackAddressCheck.cpp
SdcNoThrowStackAddressCheck.h
SdcNoTypeidPolymorphicCheck.cpp
SdcNoTypeidPolymorphicCheck.h
SdcNoUnionCheck.cpp
SdcNoUnionCheck.h
SdcNoVariableShadowingCheck.cpp
SdcNoVariableShadowingCheck.h
SdcNumericAssignmentCheck.cpp
SdcNumericAssignmentCheck.h
SdcOverrideDefaultArgCheck.cpp
SdcOverrideDefaultArgCheck.h
SdcPreprocessorUtils.h
SdcProhibitedFunctionsCheck.cpp
SdcProhibitedFunctionsCheck.h
SdcReservedNamespaceDefinitionCheck.cpp
SdcReservedNamespaceDefinitionCheck.h
SdcReturnValueUsedCheck.cpp
SdcReturnValueUsedCheck.h
SdcSpecialMemberFunctionsCheck.cpp
SdcSpecialMemberFunctionsCheck.h
SdcStdMoveNonConstLvalueCheck.cpp
SdcStdMoveNonConstLvalueCheck.h
SdcStringLiteralConcatenationCheck.cpp
SdcStringLiteralConcatenationCheck.h
SdcSwitchStructureCheck.cpp
SdcSwitchStructureCheck.h
SdcUnreachableCodeCheck.cpp
SdcUnreachableCodeCheck.h
SdcUnscopedEnumNumericUseCheck.cpp
SdcUnscopedEnumNumericUseCheck.h
SdcUseNullptrCheck.cpp
SdcUseNullptrCheck.h
SdcVirtualBaseCastViaDynamicCheck.cpp
SdcVirtualBaseCastViaDynamicCheck.h
SdcVirtualFinalNonOverrideCheck.cpp
SdcVirtualFinalNonOverrideCheck.h
SdcVirtualPMFNullCompareCheck.cpp
SdcVirtualPMFNullCompareCheck.h
SdcVolatileAppropriateCheck.cpp
SdcVolatileAppropriateCheck.h
tidy_module_sdc.cpp
{% endblock %}

{% block cmake_flags %}
{{super()}}
CMAKE_CXX_FLAGS="-DIX_CLANG_TIDY_BUILD=1"
LLVM_ENABLE_PROJECTS="llvm;clang;clang-tools-extra"
CLANG_PLUGIN_SUPPORT=FALSE
NATIVE_CLANG_DIR=$NATIVE_CLANG_DIR
{% endblock %}

{% block tidy_patches %}
dont-triggers-on-class-decls.patch
{% endblock %}

{% block patch %}
{{super()}}

{% for p in self.tidy_patches().strip().split() %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('//clang-tidy/20_sdc/patches/' + p) | b64e}}
EOF
{% endfor %}


# Adjust cross compilation
base64 -d << EOF | patch -p1 --directory=${tmp}/src
{% include 'cross.diff/base64' %}
EOF
# Disable zstd (also need for cross compilation)
echo > ${tmp}/src/llvm/cmake/modules/Findzstd.cmake

YANDEX_TIDY_MODULE=${tmp}/src/clang-tools-extra/clang-tidy/yandex/
mkdir $YANDEX_TIDY_MODULE

# Copy sources into llvm
{% for check_src in self.check_srcs().strip().split() %}
base64 -d << EOF > $YANDEX_TIDY_MODULE/{{check_src}}
{{ix.load_file('//clang-tidy/checks/' + check_src) | b64e }}
EOF
{% endfor %}

{% for check_src in self.sdc_check_srcs().strip().split() %}
base64 -d << EOF > $YANDEX_TIDY_MODULE/{{check_src}}
{{ix.load_file('//clang-tidy/20_sdc/checks/' + check_src) | b64e }}
EOF
{% endfor %}

# Create new one more module
cat << EOF > $YANDEX_TIDY_MODULE/CMakeLists.txt
set(LLVM_LINK_COMPONENTS FrontendOpenMP Support)


add_clang_library(clangTidyYandexModule

{% for check_src in self.check_srcs().strip().split() %}
{{check_src}}
{% endfor %}

{% for check_src in self.sdc_check_srcs().strip().split() %}
{{check_src}}
{% endfor %}

  LINK_LIBS clangTidy clangTidyUtils
  DEPENDS omp_gen ClangDriverOptions
)

clang_target_link_libraries(clangTidyYandexModule
  PRIVATE clangAST clangASTMatchers clangBasic clangLex
)
EOF

# Create anchor for linker
cat << EOF >> ${tmp}/src/clang-tools-extra/clang-tidy/ClangTidyForceLinker.h
#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CLANGTIDYFORCELINKER_H_ADDITION
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CLANGTIDYFORCELINKER_H_ADDITION
namespace clang::tidy {
// This anchor is used to force the linker to link the YandexModule.
extern volatile int YandexModuleAnchorSource;
static int LLVM_ATTRIBUTE_UNUSED YandexModuleAnchorDestination = YandexModuleAnchorSource;
}
#endif
EOF

# Register our module in CMake
base64 -d << EOF | patch -p1 --directory=${tmp}/src
{% include 'register_yandex.diff/base64' %}
EOF

# Register clang-static-analyzer
base64 -d << EOF >> ${tmp}/src/clang/include/clang/StaticAnalyzer/Checkers/Checkers.td
{{ix.load_file('//clang-tidy/20_sdc/patches/options.td') | b64e }}
EOF

{% endblock %}


{% block install %}
{{super()}}
mkdir -p ${out}/fix
cat << EOF > ${out}/fix/remove_unused.sh
mkdir bin1

mv bin/yaml2json* bin1/.
mv bin/clang-tidy* bin1/.
mv bin/clang-apply-replacements* bin1/.
rm -rf bin

mv bin1 bin
EOF
{% endblock %}





