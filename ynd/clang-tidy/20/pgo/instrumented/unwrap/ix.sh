{% extends '//clang-tidy/20/unwrap/ix.sh' %}

{% block bld_libs %}
{{super()}}
ynd/lib/compiler_rt/profile/21
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_BUILD_INSTRUMENTED=ON
{% endblock %}

{% block keep_tools %}
{{super()}}
mv bin/llvm-profdata* bin1/.
{% endblock %}
