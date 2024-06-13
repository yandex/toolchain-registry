{% extends '//die/hub.sh' %}

{% block lib_deps %}
{% if linux %}
lib/compiler_rt/profile/18
{% endif %}
{% endblock %}
