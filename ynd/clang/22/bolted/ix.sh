{% extends '//clang/22/template.sh' %}

{% block bld_tool %}
{{super()}}
bin/bolt/22
{% endblock %}

{% block bld_libs %}
{{super()}}
clang/22/pgo/train
clang/22/bolted/train
{% endblock %}

{% block cmake_flags %}
{{super()}}
CMAKE_EXE_LINKER_FLAGS="-Wl,--emit-relocs"
LLVM_ENABLE_LTO=Thin
LLVM_PROFDATA_FILE=$MERGED_PROFILE
{% endblock %}

{% block postinstall %}
mv ${out}/bin/clang-22 ${out}/bin/clang-22.orig

llvm-bolt ${out}/bin/clang-22.orig -o ${out}/bin/clang-22 \
 -reorder-blocks=ext-tsp -reorder-functions=hfsort+ -split-functions -split-all-cold \
 -data=$MERGED_BOLT_PROFILE

rm -rf ${out}/bin/clang-22.orig
{{super()}}
{% endblock %}
