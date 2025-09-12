{% extends '//ynd/clang/20/template.sh' %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_PROJECTS="clang;flang"
{% endblock %}

{% block llvm_targets %}
flang
{% endblock %}

