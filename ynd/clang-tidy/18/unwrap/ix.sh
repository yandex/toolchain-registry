{% extends '//clang/18/template.sh' %}


{% block bld_deps %}
{{super()}}
//clang/18
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
tidy_module.cpp
uneeded_temporary_string_check.cpp
uneeded_temporary_string_check.h
usage_restriction_checks.cpp
usage_restriction_checks.h
util_tstring_methods.cpp
util_tstring_methods.h
possible_nullptr_check.cpp
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

cd ${tmp}/src
{% for p in self.tidy_patches().strip().split() %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('//clang-tidy/18/patches/' + p) | b64e}}
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
{{ix.load_file('//clang-tidy/18/checks/' + check_src) | b64e }}
EOF
{% endfor %}

# Create new one more module
cat << EOF > $YANDEX_TIDY_MODULE/CMakeLists.txt
set(LLVM_LINK_COMPONENTS FrontendOpenMP Support)


add_clang_library(clangTidyYandexModule

{% for check_src in self.check_srcs().strip().split() %}
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
{{ix.load_file('//clang-tidy/18/patches/options.td') | b64e }}
EOF

{% endblock %}


{% block install %}
{{super()}}
mkdir -p ${out}/fix
cat << EOF > ${out}/fix/remove_unused.sh
mkdir bin1

mv bin/clang-tidy* bin1/.
mv bin/clang-apply-replacements* bin1/.
rm -rf bin

mv bin1 bin
EOF
{% endblock %}





