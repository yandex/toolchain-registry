{% extends '//clang/16/template.sh' %}

{% block bld_libs %}
{{super()}}
clang/16/pgo/train
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_LTO=Thin
LLVM_PROFDATA_FILE=$MERGED_PROFILE
{% endblock %}

