{% extends '//clang-tidy/20/unwrap/ix.sh' %}

{% block eats_check_srcs %}
base_class_special_members_check.cpp
base_class_special_members_check.h
bridge_header.h
class_size_check.cpp
class_size_check.h
empty_catch_all_check.cpp
empty_catch_all_check.h
pragma_once_check.cpp
pragma_once_check.h
tidy_module_eats.cpp
virtual_class_destructor_check.cpp
virtual_class_destructor_check.h
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
