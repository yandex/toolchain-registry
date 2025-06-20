{% extends 'template.sh' %}

{% block env %}
export NATIVE_CLANG_DIR="${out}/bin/"
{% endblock %}
