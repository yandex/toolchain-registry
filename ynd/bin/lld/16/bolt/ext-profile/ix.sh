{% extends '//bin/lld/16/ix.sh' %}

{% block bld_tool %}
{{super()}}
bin/bolt
bin/lld/16/profiles/bolt
bin/lld/16/profiles/pgo
{% endblock %}

{% block cmake_flags %}
{{super()}}
CMAKE_EXE_LINKER_FLAGS="-Wl,--emit-relocs"
LLVM_ENABLE_LTO=Thin
{% endblock %}

{% block postinstall %}
{{super()}}
mv ${out}/bin/lld ${out}/bin/lld.orig
llvm-bolt ${out}/bin/lld.orig -o ${out}/bin/lld \
 -reorder-blocks=ext-tsp -reorder-functions=hfsort+ -split-functions -split-all-cold \
 -data=${LLD_BOLT_EXTERNAL_PROFILE}

rm -rf ${out}/bin/lld.orig
{% endblock %}

