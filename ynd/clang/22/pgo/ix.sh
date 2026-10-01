{% extends '//clang/22/template.sh' %}

{% block bld_tool %}
bin/clang/22
{% endblock %}

{% block bld_libs %}
{{super()}}
clang/22/pgo/train
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_LTO=Thin
LLVM_PROFDATA_FILE=$MERGED_PROFILE
{% endblock %}
