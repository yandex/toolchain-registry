#include "bridge_header.h"

#include "base_class_special_members_check.h"
#include "class_size_check.h"
#include "default_lambda_capture_check.h"
#include "empty_catch_all_check.h"
#include "header_constant_inline_check.h"
#include "lambda_size_check.h"
#include "manual_mutex_lock_check.h"
#include "memory_order_check.h"
#include "namespace_scope_static_check.h"
#include "parameter_count_check.h"
#include "pragma_once_check.h"
#include "virtual_class_destructor_check.h"
#include "size_type_check.h"
#include "string_view_field_check.h"
#include "todo_format_check.h"
#include "unscoped_enum_check.h"
#include "using_namespace_outside_function_check.h"
#include "variant_visit_check.h"
#include "volatile_check.h"

namespace clang::tidy::eats {

class EatsModule : public ClangTidyModule {
public:
    void addCheckFactories(ClangTidyCheckFactories& checkFactories) override {
        checkFactories.registerCheck<BaseClassSpecialMembersCheck>("eats-base-class-special-members");
        checkFactories.registerCheck<ClassSizeCheck>("eats-class-size");
        checkFactories.registerCheck<DefaultLambdaCaptureCheck>("eats-default-lambda-capture");
        checkFactories.registerCheck<EmptyCatchAllCheck>("eats-empty-catch-all");
        checkFactories.registerCheck<HeaderConstantInlineCheck>("eats-header-constant-inline");
        checkFactories.registerCheck<LambdaSizeCheck>("eats-lambda-size");
        checkFactories.registerCheck<ManualMutexLockCheck>("eats-manual-mutex-lock");
        checkFactories.registerCheck<MemoryOrderCheck>("eats-memory-order");
        checkFactories.registerCheck<NamespaceScopeStaticCheck>("eats-namespace-scope-static");
        checkFactories.registerCheck<ParameterCountCheck>("eats-parameter-count");
        checkFactories.registerCheck<PragmaOnceCheck>("eats-pragma-once");
        checkFactories.registerCheck<VirtualClassDestructorCheck>("eats-virtual-class-destructor");
        checkFactories.registerCheck<SizeTypeCheck>("eats-size-type");
        checkFactories.registerCheck<StringViewFieldCheck>("eats-string-view-field");
        checkFactories.registerCheck<TodoFormatCheck>("eats-todo-format");
        checkFactories.registerCheck<UnscopedEnumCheck>("eats-unscoped-enum");
        checkFactories.registerCheck<UsingNamespaceOutsideFunctionCheck>("eats-using-namespace-outside-function");
        checkFactories.registerCheck<VariantVisitCheck>("eats-variant-visit");
        checkFactories.registerCheck<VolatileCheck>("eats-volatile");
    }
};

static ClangTidyModuleRegistry::Add<EatsModule> X("eats-module", "Adds Yandex Eats codestyle checks.");

}  // namespace clang::tidy::eats

namespace clang::tidy {
// This anchor is used to force the linker to link in the generated object file
// and thus register the EatsModule.
volatile int EatsModuleAnchorSource = 0;
}  // namespace clang::tidy
