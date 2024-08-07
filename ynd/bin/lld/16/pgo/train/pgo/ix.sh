{% extends '//clang/16/ix.sh' %}

{% block bld_tool %}
bin/lld/16/pgo/instrumented
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_LTO=Thin
{% endblock %}

{% block setup_tools %}
cat << EOF > wrapped_lld
#!/usr/bin/env sh
export LLVM_PROFILE_FILE=${out}/profiles/default%m.profraw
exec ${INSTR_LLD_PATH} "\${@}"
EOF
chmod +x wrapped_lld
{{super()}}
{% endblock %}

{% block setup_target_flags %}
{{super()}}
export LDFLAGS="--ld-path=${tmp}/bin/ut/wrapped_lld ${LDFLAGS}"
{% endblock %}

{% block postinstall %}
rm -rf ${out}/bin ${out}/lib ${out}/share ${out}/include
{% endblock %}

{% block env %}
export LLD_PGO_PROFILES=${out}/profiles
{% endblock %}

