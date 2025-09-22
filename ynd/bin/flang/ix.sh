{% extends '//ynd/clang/20/template.sh' %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_PROJECTS="clang;flang"
CMAKE_CROSSCOMPILING=NO
{% endblock %}

{% block common_patches %}
fix-flang-build-win32.patch
llvm-21-mmacosx-version-min.patch
{% endblock %}

{% block llvm_targets %}
flang
{% endblock %}

