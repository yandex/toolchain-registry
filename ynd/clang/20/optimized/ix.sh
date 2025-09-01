{% extends '//die/hub.sh' %}

{% block run_deps %}
{% if linux and x86_64 %}
clang/20/bolted/ext-profile
{% elif mingw32 %}
clang/20
{% else %}
clang/20/pgo/ext-profile
{% endif %}
{% endblock %}
