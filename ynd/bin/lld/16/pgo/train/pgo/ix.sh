{% extends '//clang/16/pgo/ext-profile/ix.sh' %}

{% block bld_tool %}
bin/lld/16/pgo/instrumented
{% endblock %}

{% block configure %}
rm -rf ${LLD_PROFILES_DIR}/*
{{super()}}
{% endblock %}

{% block postinstall %}
echo "Copy profiles"
mkdir ${out}/profiles
cp ${LLD_PROFILES_DIR}/*.profraw ${out}/profiles
rm -rf ${out}/bin ${out}/lib ${out}/share ${out}/include
{% endblock %}

{% block env %}
export LLD_PGO_PROFILES=${out}/profiles
{% endblock %}

