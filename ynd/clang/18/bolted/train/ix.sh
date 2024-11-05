{% extends '//clang/18/template.sh' %}

{% block bld_tool %}
{{super()}}
bin/perf
bin/bolt/19
ynd/clang/18/pgo
{% endblock %}

{% block build %}

cc_binary=$(readlink -f $(cat $(which ${CXX}) | grep -o '/.*/clang'))
echo $cc_binary
cp $cc_binary ${out}/clang-18

perf record -o ${out}/perf.data -c 10000 -e cycles:u -j any,u -- \
ninja -C {{ninja_build_dir}} -j {% block ninja_threads %}${make_thrs}{% endblock %} {{ix.fix_list(ninja_build_targets)}}
{% endblock %}

{% block install %}
{{super()}}

echo "Prepare for profile convertion"
perf2bolt -p ${out}/perf.data -o ${out}/bolt.fdata ${out}/clang-18
merge-fdata ${out}/bolt.fdata > ${out}/bolt.prof
ls ${out} -alth

rm -rf ${out}/perf.data ${out}/bolt.fdata ${out}/clang-18
{% endblock %}

{% block env %}
export MERGED_BOLT_PROFILE=${out}/bolt.prof
{% endblock %}
