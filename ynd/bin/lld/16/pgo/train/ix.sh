{% extends '//clang/16/ix.sh' %}

{% block bld_tool %}
bin/lld/16/pgo/instrumented
bin/llvm-profdata
{% endblock %}

{% block postinstall %}
echo "Merge profiles"
llvm-profdata merge --output=${out}/merged.prof ${LLD_PROFILES_DIR}/*.profraw
rm -rf ${out}/bin ${out}/lib ${out}/share ${out}/include
{% endblock %}

{% block env %}
export MERGED_PROFILE=${out}/merged.prof
{% endblock %}

