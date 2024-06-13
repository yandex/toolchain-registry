{% extends '//clang/16/template.sh' %}

{% block bld_data %}
clang/16/profiles/pgo
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_LTO=Thin
LLVM_PROFDATA_FILE=$PGO_EXTERNAL_PROFILE
{% endblock %}

