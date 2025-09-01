{% extends '//clang/20/template.sh' %}

{% block bld_data %}
clang/20/profiles
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_LTO=Thin
LLVM_PROFDATA_FILE=$PGO_EXTERNAL_PROFILE
{% endblock %}
