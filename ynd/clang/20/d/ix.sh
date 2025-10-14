{% extends '//clang/20/template.sh' %}

{% block bld_tool %}
{{super()}}
ynd/lib/llvm/20/tblgen
{% endblock %}

{% block llvm_projects %}
{{super()}}
clang-tools-extra
{% endblock %}

{% block llvm_targets %}
clangd
clang-resource-headers
{% endblock %}

{% block cmake_flags %}
{{super()}}
CMAKE_CROSSCOMPILING=NO
{% endblock %}

{% block postinstall %}
:
{% endblock %}

{% block clang_fix_includes %}
{% endblock %}
