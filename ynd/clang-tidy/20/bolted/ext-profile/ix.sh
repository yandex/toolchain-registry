{% extends '//clang-tidy/20/pgo/unwrap/ix.sh' %}

{% block bld_tool %}
{{super()}}
ynd/bin/bolt/20
{% endblock %}

{% block bld_data %}
{{super()}}
ynd/clang-tidy/20/bolted/profiles
{% endblock %}

{% block install %}
{{super()}}
mv ${out}/bin/clang-tidy ${out}/bin/clang-tidy.prebolt
llvm-bolt ${out}/bin/clang-tidy.prebolt -o ${out}/bin/clang-tidy \
    -reorder-blocks=ext-tsp -reorder-functions=hfsort+ \
    -split-functions -split-all-cold -data=${CLANG_TIDY_BOLT_PROFILE}
rm ${out}/bin/clang-tidy.prebolt
{% endblock %}
