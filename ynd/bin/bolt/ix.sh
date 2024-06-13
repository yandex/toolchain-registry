{% extends '//bin/clang/t/t/ix.sh' %}

{% block fetch %}
{% include '//lib/llvm/18/ver.sh' %}
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_TARGETS_TO_BUILD="X86;AArch64"
LLVM_ENABLE_PROJECTS="bolt"
{% endblock %}

{% block llvm_targets %}
bolt
{% endblock %}

{% block patch %}
base64 -d << EOF | patch -p1
{% include '0.diff/base64' %}
EOF
{% endblock %}

