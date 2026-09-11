{% extends '//die/hub.sh' %}

{% block run_deps %}
{% if linux and x86_64 %}
ynd/clang-tidy/20/bolted
{% elif mingw32 %}
ynd/clang-tidy/20
{% else %}
ynd/clang-tidy/20/pgo
{% endif %}
{% endblock %}
