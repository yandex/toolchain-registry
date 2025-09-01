{% extends '//bin/clang/t/t/ix.sh' %}

{% block fetch %}
{% include '//lib/llvm/20/ver.sh' %}
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_TARGETS_TO_BUILD="X86;AArch64"
LLVM_ENABLE_PROJECTS="bolt"
{% endblock %}

{% block llvm_targets %}
bolt
{% endblock %}


