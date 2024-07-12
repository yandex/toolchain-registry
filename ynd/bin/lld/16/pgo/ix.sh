{% extends '//bin/lld/16/ix.sh' %}

{% block bld_libs %}
{{super()}}
bin/lld/16/pgo/train
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_LTO=Thin
LLVM_PROFDATA_FILE=$LLD_MERGED_PROFILE
{% endblock %}

