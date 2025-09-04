{% extends '//clang/20/template.sh' %}

{% block bld_tool %}
clang/20/pgo/instrumented
bin/llvm-profdata/19
{% endblock %}

{% block postinstall %}
echo "Merge profiles"
llvm-profdata merge --output=${out}/merged.prof $PROFILES_DIR/*.profraw
rm -rf ${out}/bin ${out}/lib ${out}/share ${out}/include
{% endblock %}

{% block env %}
export MERGED_PROFILE=${out}/merged.prof
{% endblock %}
