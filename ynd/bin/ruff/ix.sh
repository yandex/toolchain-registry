{% extends '//die/hub.sh' %}

{% block run_deps %}
{% if mingw32 %}
ynd/bin/ruff/unwrap(force_allocator=mimalloc/1)
{% else %}
ynd/bin/ruff/unwrap(force_allocator=jemalloc)
{% endif %}
{% endblock %}
