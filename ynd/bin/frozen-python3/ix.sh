{% extends '//die/hub.sh' %}

{% block run_deps %}
{% if mingw32 %}
ynd/bin/frozen-python3/win32
{% else %}
ynd/bin/frozen-python3/unwrap(python_ver=12)
{% endif %}
{% endblock %}
