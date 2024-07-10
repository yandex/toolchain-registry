{% extends '//bin/lld/16/ix.sh' %}

{% block bld_libs %}
{{super()}}
lib/compiler_rt/profile
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_PROJECTS="lld"
LLVM_ENABLE_LTO=Thin
LLVM_BUILD_INSTRUMENTED=CSIR
LLVM_BUILD_RUNTIME=NO
{% endblock %}

{% block llvm_targets %}
lld
{% endblock %}

{% block env %}
## By default lld does not call destructors. For details see
## https://github.com/llvm/llvm-project/issues/52861#issuecomment-1724038537
export LLD_IN_TEST=1
export LLD_PROFILES_DIR={{ninja_build_dir}}/csprofiles
{{super()}}
{% endblock %}

