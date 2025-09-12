{% extends '//ynd/clang/20/template.sh' %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_PROJECTS="flang"
{% endblock %}

{% block llvm_targets %}
flang
{% endblock %}

