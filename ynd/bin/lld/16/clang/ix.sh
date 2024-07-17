{% extends '//clang/16/template.sh' %}

{% block bld_libs %}
{{super()}}
lib/compiler_rt/profile
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_PROJECTS="clang"
LLVM_BUILD_RUNTIME=NO
{% endblock %}

{% block llvm_targets %}
clang
clang-resource-headers
{% endblock %}

{% block postinstall %}
mkdir -p ${out}/share
mv ${out}/lib/clang/1*/include ${out}/share/
rm -rf ${out}/lib
{% endblock %}

