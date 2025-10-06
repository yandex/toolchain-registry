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

{% block bolt_patches %}
01-pr-151927.patch
02-pr-151923.patch
{% endblock %}

{% block patch %}

{% for p in self.bolt_patches().strip().split() %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('//bin/bolt/20/patches/' + p) | b64e}}
EOF
{% endfor %}

{% endblock %}
