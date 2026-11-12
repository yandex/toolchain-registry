#include "bridge_header.h"

#include "base_class_special_members_check.h"
#include "catch_const_reference_check.h"
#include "class_size_check.h"
#include "complex_condition_check.h"
#include "class_file_name_check.h"
#include "default_lambda_capture_check.h"
#include "empty_catch_all_check.h"
#include "header_constant_inline_check.h"
#include "include_style_check.h"
#include "lambda_size_check.h"
#include "magic_string_check.h"
#include "manual_mutex_lock_check.h"
#include "memory_order_check.h"
#include "namespace_alias_in_header_check.h"
#include "namespace_scope_static_check.h"
#include "namespace_path_check.h"
#include "parameter_count_check.h"
#include "pragma_once_check.h"
#include "virtual_class_destructor_check.h"
#include "size_type_check.h"
#include "string_view_field_check.h"
#include "strong_types_check.h"
#include "todo_format_check.h"
#include "unscoped_enum_check.h"
#include "using_namespace_outside_function_check.h"
#include "variant_visit_check.h"
#include "variable_postfix_check.h"
#include "volatile_check.h"

namespace clang::tidy::eats {

    class EatsModule: public ClangTidyModule {
    public:
        // A check is named by the key of its rule in the Eats backend codestyle,
        // EATS_<AREA>_<NAME>: see taxi/eda/clang_tidy/checks/README.md.
        void addCheckFactories(ClangTidyCheckFactories& checkFactories) override {
            checkFactories.registerCheck<BaseClassSpecialMembersCheck>("EATS_CPP_BASE_CLASS_SPECIAL_MEMBERS");
            checkFactories.registerCheck<CatchConstReferenceCheck>("EATS_CPP_CATCH_CONST_REFERENCE");
            checkFactories.registerCheck<ClassFileNameCheck>("EATS_CPP_CLASS_FILE_NAME");
            checkFactories.registerCheck<ClassSizeCheck>("EATS_CPP_CLASS_SIZE");
            checkFactories.registerCheck<ComplexConditionCheck>("EATS_CPP_COMPLEX_CONDITION");
            checkFactories.registerCheck<DefaultLambdaCaptureCheck>("EATS_CPP_LAMBDA_CAPTURE");
            checkFactories.registerCheck<EmptyCatchAllCheck>("EATS_CPP_EMPTY_CATCH_ALL");
            checkFactories.registerCheck<HeaderConstantInlineCheck>("EATS_CPP_HEADER_CONSTANTS");
            checkFactories.registerCheck<IncludeStyleCheck>("EATS_CPP_INCLUDE_STYLE");
            checkFactories.registerCheck<LambdaSizeCheck>("EATS_CPP_LAMBDA_SIZE");
            checkFactories.registerCheck<MagicStringCheck>("EATS_CPP_MAGIC_LITERALS");
            checkFactories.registerCheck<ManualMutexLockCheck>("EATS_CPP_MUTEX_LOCKING");
            checkFactories.registerCheck<MemoryOrderCheck>("EATS_CPP_MEMORY_ORDER");
            checkFactories.registerCheck<NamespaceAliasInHeaderCheck>("EATS_CPP_NAMESPACE_ALIAS");
            checkFactories.registerCheck<NamespacePathCheck>("EATS_CPP_NAMESPACE_PATH");
            checkFactories.registerCheck<NamespaceScopeStaticCheck>("EATS_CPP_ANONYMOUS_NAMESPACE");
            checkFactories.registerCheck<ParameterCountCheck>("EATS_COMMON_MAX_PARAMS");
            checkFactories.registerCheck<PragmaOnceCheck>("EATS_CPP_PRAGMA_ONCE");
            checkFactories.registerCheck<VirtualClassDestructorCheck>("EATS_CPP_VIRTUAL_DESTRUCTOR");
            checkFactories.registerCheck<SizeTypeCheck>("EATS_CPP_INTEGER_TYPES");
            checkFactories.registerCheck<StringViewFieldCheck>("EATS_CPP_STRING_VIEW_LIFETIME");
            checkFactories.registerCheck<StrongTypesCheck>("EATS_CPP_STRONG_TYPES");
            checkFactories.registerCheck<TodoFormatCheck>("EATS_CPP_TODO_FORMAT");
            checkFactories.registerCheck<UnscopedEnumCheck>("EATS_CPP_ENUM_CLASS");
            checkFactories.registerCheck<UsingNamespaceOutsideFunctionCheck>("EATS_CPP_USING_NAMESPACE");
            checkFactories.registerCheck<VariantVisitCheck>("EATS_CPP_VARIANT_VISIT");
            checkFactories.registerCheck<VariablePostfixCheck>("EATS_CPP_VARIABLE_SUFFIXES");
            checkFactories.registerCheck<VolatileCheck>("EATS_CPP_VOLATILE");
        }
    };

    static ClangTidyModuleRegistry::Add<EatsModule> X("eats-module", "Adds Yandex Eats codestyle checks.");

} // namespace clang::tidy::eats

namespace clang::tidy {
    // This anchor is used to force the linker to link in the generated object file
    // and thus register the EatsModule.
    volatile int EatsModuleAnchorSource = 0;
} // namespace clang::tidy
