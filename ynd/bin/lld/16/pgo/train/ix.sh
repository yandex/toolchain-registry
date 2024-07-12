{% extends '//die/proxy.sh' %}

{% block bld_tool %}
bin/lld/16/pgo/train/pgo
bin/lld/16/pgo/train/simple
bin/llvm-profdata
{% endblock %}

{% block postinstall %}
echo "Merge profiles"
llvm-profdata merge --output=${out}/merged.prof ${LLD_PGO_PROFILES}/*.profraw ${LLD_SIMPLE_PROFILES}/*.profraw
rm -rf ${out}/bin ${out}/lib ${out}/share ${out}/include
{% endblock %}

{% block env %}
export LLD_MERGED_PROFILE=${out}/merged.prof
{% endblock %}

