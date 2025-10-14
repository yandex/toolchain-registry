{% extends '//clang/18/template.sh' %}

{% block bld_tool %}
ynd/bin/compiler/huge_stack
{{super()}}
bin/bolt/19
{% endblock %}

{% block bld_data %}
clang/18/profiles
{% endblock %}

{% block cmake_flags %}
{{super()}}
CMAKE_EXE_LINKER_FLAGS="-Wl,--emit-relocs"
LLVM_ENABLE_LTO=Thin
LLVM_PROFDATA_FILE=$PGO_EXTERNAL_PROFILE
{% endblock %}

{% block postinstall %}
mv ${out}/bin/clang-18 ${out}/bin/clang-18.orig

llvm-bolt ${out}/bin/clang-18.orig -o ${out}/bin/clang-18 \
 -reorder-blocks=ext-tsp -reorder-functions=hfsort+ -split-functions -split-all-cold \
 -data=$BOLT_EXTERNAL_PROFILE 

rm -rf ${out}/bin/clang-18.orig
{{super()}}
{% endblock %}
