{% extends '//die/hub.sh' %}

{% block run_deps %}
{% if linux and x86_64 %}
clang/22/bolted/ext-profile
{% elif mingw32 %}
clang/22
{% else %}
clang/22/pgo/ext-profile
{% endif %}
{% endblock %}
