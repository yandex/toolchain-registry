{% extends '//die/hub.sh' %}

{% block run_deps %}
ynd/clang-tidy/20_sdc/unwrap(jail=)
ynd/bin/yaml2json
{% endblock %}
