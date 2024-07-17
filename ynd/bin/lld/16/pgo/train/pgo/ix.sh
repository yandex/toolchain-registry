{% extends '//clang/16/ix.sh' %}

{% block bld_tool %}
bin/lld/16/clang
bin/lld/16/pgo/instrumented
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_LTO=Thin
{% endblock %}

{% block configure %}
export LDFLAGS="--ld-path=${INSTR_LLD_PATH} ${LDFLAGS}"
{{super()}}
{% endblock %}

{% block build %}
rm -rf ${LLD_PROFILES_DIR}/*
{{super()}}
{% endblock %}

{% block postinstall %}
echo "Copy profiles"
mkdir ${out}/profiles
mv ${LLD_PROFILES_DIR}/*.profraw ${out}/profiles
rm -rf ${LLD_PROFILES_DIR}
rm -rf ${out}/bin ${out}/lib ${out}/share ${out}/include
{% endblock %}

{% block env %}
export LLD_PGO_PROFILES=${out}/profiles
{% endblock %}

