{% extends '//bin/lld/16/ix.sh' %}

{% block bld_data %}
bin/lld/16/profiles/pgo
{% endblock %}

{% block patch %}
base64 -d << EOF | patch -p1
{% include 'emit-compact-unwind-only-for-canonical.diff/base64' %}
EOF
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_LTO=Thin
LLVM_PROFDATA_FILE=$PGO_EXTERNAL_PROFILE
{% endblock %}

{% block env %}
export LLD_EXT_PROFILE_PATH=${out}/bin/ld.lld
{% endblock %}
