{% extends '//die/hub.sh' %}

{% block run_deps %}
{% if linux and x86_64 %}
clang/22/pgo
{% else %}
clang/22
{% endif %}
{% endblock %}
