{% extends '//die/hub.sh' %}

{% block run_deps %}
bld/rust/96/{{target.os}}
{% if linux %}
bld/rust/96/musl
{% endif %}
{% endblock %}
