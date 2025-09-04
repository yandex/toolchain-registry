{% extends '//clang/20/template.sh' %}

{% block bld_libs %}
{{super()}}
ynd/lib/compiler_rt/profile/19
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_PROJECTS="clang"
LLVM_BUILD_INSTRUMENTED=ON
{% endblock %}

{% block llvm_targets %}
clang
clang-resource-headers
{% endblock %}

{% block postinstall %}
mkdir -p ${out}/share
mv ${out}/lib/clang/2*/include ${out}/share/
rm -rf ${out}/lib
{% endblock %}

{% block env %}
export PROFILES_DIR={{ninja_build_dir}}/profiles
{{super()}}
{% endblock %}

