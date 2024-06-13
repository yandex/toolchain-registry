{% extends '//die/hub.sh' %}

{% block run_deps %}
{% if linux and x86_64 %}
clang/16/bolted/ext-profile
{% elif mingw32 %}
clang/16
{% else %}
clang/16/pgo/ext-profile
{% endif %}
{% endblock %}
