{% extends '//clang/22/template.sh' %}

{% block bld_data %}
clang/22/profiles
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_LTO=Thin
LLVM_PROFDATA_FILE=$PGO_EXTERNAL_PROFILE
{% endblock %}
