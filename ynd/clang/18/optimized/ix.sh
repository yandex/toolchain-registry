{% extends '//die/hub.sh' %}

{% block run_deps %}
{% if linux and x86_64 %}
clang/18/bolted/ext-profile
{% elif mingw32 %}
clang/18
{% else %}
clang/18
{% endif %}
{% endblock %}
