{% extends '//die/hub.sh' %}

{% block run_deps %}
{% if linux and x86_64 %}
clang/16/bolted/ext-profile
{% else %}
clang/16/pgo/ext-profile
{% endif %}
{% endblock %}
