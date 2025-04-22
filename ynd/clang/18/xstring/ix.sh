{% extends '//clang/18/template.sh' %}

{% block clang_patches %}
{{super()}}
clang-xstring.patch
{% endblock %}

