{% extends '//clang-tidy/20/unwrap/ix.sh' %}

{% block bld_data %}
{{super()}}
clang-tidy/20/pgo/profiles
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_LTO=Thin
{% if linux and x86_64 %}
CMAKE_EXE_LINKER_FLAGS="-Wl,--emit-relocs"
{% endif %}
LLVM_PROFDATA_FILE=$CLANG_TIDY_PGO_PROFILE
{% endblock %}

{% block keep_tools %}
{{super()}}
{% for tool in ('llvm-bolt', 'perf2bolt') %}
if test -e bin/{{tool}} || test -L bin/{{tool}}; then
    mv bin/{{tool}} bin1/.
fi
{% endfor %}
{% endblock %}
