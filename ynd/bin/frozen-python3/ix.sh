{% extends '//die/hub.sh' %}

{% block run_deps %}
ynd/bin/frozen-python3/unwrap(python_ver=12)
{% endblock %}
