{% extends '//bin/clang/18/ix.sh' %}

{% block bld_tool %}
lib/llvm/18/tblgen
{{super()}}
{% endblock %}

{% block cmake_flags %}
{{super()}}
{% if linux and x86_64 %}
LLVM_ENABLE_PROJECTS="clang;clang-tools-extra;lld;polly"
{% endif %}
LLVM_ENABLE_LIBXML2="FORCE_ON"
{% endblock %}

{% block llvm_targets %}
{{super()}}
clang-format
clang-rename
{% if linux and x86_64 %}
clang-tidy
{% endif %}
llvm-cov
llvm-profdata
llvm-rc
sancov
{% endblock %}

{% block bld_libs %}
lib/xml/2
{{super()}}
{% endblock %}

{% block postinstall %}
cd ${out}/bin
{% if linux and x86_64 %}
ln -s llvm-cov llvm-gcov
{% endif %}
{% if mingw32 %}
ln -s llvm-ar.exe llvm-lib.exe
{% else %}
ln -s llvm-ar llvm-lib
{% endif %}
{% endblock %}

{% block clang_fix_includes %}
:
{% endblock %}
