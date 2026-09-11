{% extends '//die/hub.sh' %}

{% block run_deps %}
ynd/clang-tidy/20/bolted/ext-profile(jail=)
ynd/bin/yaml2json
{% endblock %}
