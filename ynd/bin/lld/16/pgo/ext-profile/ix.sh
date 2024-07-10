{% extends '//bin/lld/16/ix.sh' %}

{% block bld_data %}
bin/lld/16/profiles/pgo
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_LTO=Thin
LLVM_PROFDATA_FILE=$PGO_EXTERNAL_PROFILE
{% endblock %}

