{% extends '//clang-tidy/20/unwrap/ix.sh' %}

{% block eats_check_srcs %}
base_class_special_members_check.cpp
base_class_special_members_check.h
bridge_header.h
class_size_check.cpp
class_size_check.h
default_lambda_capture_check.cpp
default_lambda_capture_check.h
empty_catch_all_check.cpp
empty_catch_all_check.h
header_constant_inline_check.cpp
header_constant_inline_check.h
lambda_size_check.cpp
lambda_size_check.h
manual_mutex_lock_check.cpp
manual_mutex_lock_check.h
memory_order_check.cpp
memory_order_check.h
namespace_scope_static_check.cpp
namespace_scope_static_check.h
parameter_count_check.cpp
parameter_count_check.h
pragma_once_check.cpp
pragma_once_check.h
size_type_check.cpp
size_type_check.h
string_view_field_check.cpp
string_view_field_check.h
tidy_module_eats.cpp
virtual_class_destructor_check.cpp
virtual_class_destructor_check.h
todo_format_check.cpp
todo_format_check.h
unscoped_enum_check.cpp
unscoped_enum_check.h
using_namespace_outside_function_check.cpp
using_namespace_outside_function_check.h
variant_visit_check.cpp
variant_visit_check.h
volatile_check.cpp
volatile_check.h
{% endblock %}

{% block patch %}
{{super()}}

EATS_TIDY_MODULE=${tmp}/src/clang-tools-extra/clang-tidy/eats/
mkdir $EATS_TIDY_MODULE

{% for check_src in self.eats_check_srcs().strip().split() %}
base64 -d << EOF > $EATS_TIDY_MODULE/{{check_src}}
{{ix.load_file('//clang-tidy/20_eats/checks/' + check_src) | b64e }}
EOF
{% endfor %}

cat << EOF > $EATS_TIDY_MODULE/CMakeLists.txt
set(LLVM_LINK_COMPONENTS FrontendOpenMP Support)


add_clang_library(clangTidyEatsModule

{% for check_src in self.eats_check_srcs().strip().split() %}
{{check_src}}
{% endfor %}

  LINK_LIBS clangTidy clangTidyUtils
  DEPENDS omp_gen ClangDriverOptions
)

clang_target_link_libraries(clangTidyEatsModule
  PRIVATE clangAST clangASTMatchers clangBasic clangLex
)
EOF

cat << EOF >> ${tmp}/src/clang-tools-extra/clang-tidy/ClangTidyForceLinker.h
#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CLANGTIDYFORCELINKER_H_EATS_ADDITION
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CLANGTIDYFORCELINKER_H_EATS_ADDITION
namespace clang::tidy {
// This anchor is used to force the linker to link the EatsModule.
extern volatile int EatsModuleAnchorSource;
static int LLVM_ATTRIBUTE_UNUSED EatsModuleAnchorDestination = EatsModuleAnchorSource;
}
#endif
EOF

base64 -d << EOF | patch -p1 --directory=${tmp}/src
{% include 'register_eats.diff/base64' %}
EOF
{% endblock %}
