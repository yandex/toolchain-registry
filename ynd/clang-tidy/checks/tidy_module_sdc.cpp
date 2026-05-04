#include "bridge_header.h"

#include "taxi_async_use_after_free_check.h"
#include "taxi_coroutine_unsafe_check.h"
#include "taxi_dangling_config_ref_check.h"

#include "ascii_compare_ignore_case_check.h"
#include "uneeded_temporary_string_check.h"
#include "usage_restriction_checks.h"
#include "using_namespace_in_header_check.h"
#include "util_tstring_methods.h"

#include "SdcDynamicMemoryAutomaticCheck.h"
#include "SdcGetenvPointerConstQualifiedCheck.h"
#include "SdcNoAdvancedMemoryManagementCheck.h"
#include "SdcNoAtoFunctionsCheck.h"
#include "SdcNoLowercaseLSuffixCheck.h"
#include "SdcNoMemFunctionsCheck.h"
#include "SdcNoOctalConstantsCheck.h"
#include "SdcNoSetlocaleGlobalCheck.h"
#include "SdcNoStaticLocalVariablesCheck.h"
#include "SdcNoStringFunctionsCheck.h"
#include "SdcNoSystemCheck.h"
#include "SdcReturnValueUsedCheck.h"
#include "SdcSpecialMemberFunctionsCheck.h"
#include "SdcStdMoveNonConstLvalueCheck.h"

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

        CheckFactories.registerCheck<sdc::SdcDynamicMemoryAutomaticCheck>("sdc-dynamic-memory-automatic");
        CheckFactories.registerCheck<sdc::SdcGetenvPointerConstQualifiedCheck>("sdc-getenv-pointer-const-qualified");
        CheckFactories.registerCheck<sdc::SdcNoAdvancedMemoryManagementCheck>("sdc-no-advanced-memory-management");
        CheckFactories.registerCheck<sdc::SdcNoAtoFunctionsCheck>("sdc-no-ato-functions");
        CheckFactories.registerCheck<sdc::SdcNoLowercaseLSuffixCheck>("sdc-no-lowercase-l-suffix");
        CheckFactories.registerCheck<sdc::SdcNoMemFunctionsCheck>("sdc-no-mem-functions");
        CheckFactories.registerCheck<sdc::SdcNoOctalConstantsCheck>("sdc-no-octal-constants");
        CheckFactories.registerCheck<sdc::SdcNoSetlocaleGlobalCheck>("sdc-no-setlocale-global");
        CheckFactories.registerCheck<sdc::SdcNoStaticLocalVariablesCheck>("sdc-no-static-local-variables");
        CheckFactories.registerCheck<sdc::SdcNoStringFunctionsCheck>("sdc-no-string-functions");
        CheckFactories.registerCheck<sdc::SdcNoSystemCheck>("sdc-no-system");
        CheckFactories.registerCheck<sdc::SdcReturnValueUsedCheck>("sdc-return-value-used");
        CheckFactories.registerCheck<sdc::SdcSpecialMemberFunctionsCheck>("sdc-special-member-functions");
        CheckFactories.registerCheck<sdc::SdcStdMoveNonConstLvalueCheck>("sdc-std-move-non-const-lvalue");
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
