{% extends '//clang/16/template.sh' %}

{% block bld_tool %}
clang/16/pgo/instrumented
bin/llvm-profdata
{% endblock %}

{% block postinstall %}
echo "Merge profiles"
llvm-profdata merge --output=${out}/merged.prof $PROFILES_DIR/*.profraw
rm -rf ${out}/bin ${out}/lib ${out}/share ${out}/include
{% endblock %}

{% block env %}
export MERGED_PROFILE=${out}/merged.prof
{% endblock %}

