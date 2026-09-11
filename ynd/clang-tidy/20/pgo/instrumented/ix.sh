{% extends '//die/hub.sh' %}

{% block run_deps %}
ynd/clang-tidy/20/pgo/instrumented/unwrap
ynd/bin/llvm-profdata/20
ynd/bin/yaml2json
{% endblock %}
