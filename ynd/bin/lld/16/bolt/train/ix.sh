{% extends '//clang/16/ix.sh' %}

{% block bld_tool %}
{{super()}}
bin/lld/16/clang
bin/lld/16/pgo/ext-profile
{% endblock %}

{% block configure %}
export LDFLAGS="--ld-path=${LLD_EXT_PROFILE_PATH} ${LDFLAGS}"
{{super()}}
{% endblock %}

{% block postinstall %}
echo ${LLD_EXT_PROFILE_PATH} > "${out}/linker_abs_path"
{% endblock %}
