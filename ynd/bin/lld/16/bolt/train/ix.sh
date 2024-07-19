{% extends '//clang/16/ix.sh' %}

{% block c_compiler %}
bld/compiler(clang_ver=16)
{% endblock %}

{% block bld_tool %}
{{super()}}
bin/lld/16/pgo/ext-profile
bin/bolt
bin/perf
{% endblock %}

{% block setup_tools %}
cat << EOF > perf_lld
#!/usr/bin/env sh
perf record --output ${tmp}/profiles/lld_perf --timestamp-filename --event cycles:u --branch-filter any,u -- ${LLD_EXT_PROFILE_PATH} "\${@}"
EOF
chmod +x perf_lld
mkdir ${tmp}/profiles
{{super()}}
{% endblock %}

{% block setup_target_flags %}
{{super()}}
export LDFLAGS="--ld-path=${tmp}/bin/ut/perf_lld ${LDFLAGS}"
{% endblock %}

{% block postinstall %}
{{super()}}
echo ${LLD_EXT_PROFILE_PATH} > ${out}/linker_abs_path
sed -i -e 's|ld.lld|lld|g' ${out}/linker_abs_path
cd ${tmp}
llvm-bolt $(find . -wholename "./profiles/lld_perf*" | while read data; do echo -p ${data}; done) -o ${out}/bolt.prof -aggregate-only $(cat ${out}/linker_abs_path)
rm -rf ${out}/bin ${out}/lib ${out}/share ${out}/include
{% endblock %}
