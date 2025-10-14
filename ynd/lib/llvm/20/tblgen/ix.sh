{% extends '//lib/llvm/16/tblgen/ix.sh' %}

{% block fetch %}
{% include '//lib/llvm/20/ver.sh' %}
{% endblock %}

{% block llvm_projects %}
{{super()}}
clang-tools-extra
{% endblock %}

{% block ninja_build_targets %}
{{super()}}
clang-tidy-confusable-chars-gen
{% endblock %}

{% block install %}
{{super()}}
cp ${tmp}/obj/bin/clang-tidy-confusable-chars-gen ${out}/bin/
{% endblock %}

{% block env %}
{{super()}}
export CMFLAGS="-DCLANG_TIDY_CONFUSABLE_CHARS_GEN=${out}/bin/clang-tidy-confusable-chars-gen \${CMFLAGS}"
{% endblock %}
