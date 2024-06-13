{% extends '//clang/16/template.sh' %}

{% block bld_tool %}
{{super()}}
bin/bolt
{% endblock %}

{% block bld_data %}
clang/16/profiles/bolt
clang/16/profiles/pgo
{% endblock %}

{% block cmake_flags %}
{{super()}}
CMAKE_EXE_LINKER_FLAGS="-Wl,--emit-relocs"
LLVM_ENABLE_LTO=Thin
LLVM_PROFDATA_FILE=$PGO_EXTERNAL_PROFILE
{% endblock %}

{% block postinstall %}
mv ${out}/bin/clang-16 ${out}/bin/clang-16.orig

llvm-bolt ${out}/bin/clang-16.orig -o ${out}/bin/clang-16 \
 -reorder-blocks=ext-tsp -reorder-functions=hfsort+ -split-functions -split-all-cold \
 -data=$BOLT_EXTERNAL_PROFILE 

rm -rf ${out}/bin/clang-16.orig
{{super()}}
{% endblock %}

