#include "bridge_header.h"

#include "taxi_async_use_after_free_check.h"
#include "taxi_coroutine_unsafe_check.h"
#include "taxi_dangling_config_ref_check.h"

#include "ascii_compare_ignore_case_check.h"
#include "uneeded_temporary_string_check.h"
#include "usage_restriction_checks.h"
#include "util_tstring_methods.h"

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
        CheckFactories.registerCheck<UtilTStringUpperCaseMethodsCheck>("arcadia-util-tstring-methods");
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
